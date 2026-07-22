/*
 * ws.h - Minimal WebSocket implementation (RFC 6455)
 * Server-side: handles Upgrade handshake, send/recv frames.
 * Client-side: sends Upgrade request, masks frames.
 *
 * Only uses text/binary frames. No ping/pong needed (we have heartbeats).
 * Zero dependencies beyond the OS socket API.
 */

#ifndef WS_H
#define WS_H

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET ws_sock_t;
  #define WS_INVALID INVALID_SOCKET
  #define ws_close(s) closesocket(s)
#else
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int ws_sock_t;
  #define WS_INVALID (-1)
  #define ws_close(s) close(s)
#endif

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Base64 (for Sec-WebSocket-Accept) ──────────────────────────────────────── */
static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const unsigned char *in, int len, char *out) {
    int i = 0, j = 0;
    while (i < len) {
        unsigned char a = i<len?in[i++]:0, b = i<len?in[i++]:0, c = i<len?in[i++]:0;
        unsigned int t = (a<<16)|(b<<8)|c;
        out[j++] = b64c[(t>>18)&63]; out[j++] = b64c[(t>>12)&63];
        out[j++] = b64c[(t>>6)&63];  out[j++] = b64c[t&63];
    }
    int pad = (3 - len%3)%3;
    for (int k=0; k<pad; k++) out[j-1-k+(pad>0?0:0)] = '=';
    /* fix: overwrite last pad chars */
    if (pad==1) out[j-1]='=';
    else if (pad==2) { out[j-2]='='; out[j-1]='='; }
    out[j] = '\0';
}

/* ── SHA-1 (for WebSocket handshake) ─────────────────────────────────────────
 * Minimal self-contained SHA-1. Only used once per connection for handshake. */
typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; int buflen; } sha1_t;
static void sha1_init(sha1_t *s) {
    s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE;
    s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0;
    s->len=0; s->buflen=0;
}
#define ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))
static void sha1_block(sha1_t *s, const unsigned char *blk) {
    uint32_t w[80], a,b,c,d,e,f,k,t;
    for(int i=0;i<16;i++) w[i]=((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|blk[i*4+3];
    for(int i=16;i<80;i++) w[i]=ROL32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];
    for(int i=0;i<80;i++){
        if(i<20){f=(b&c)|(~b&d);k=0x5A827999;}
        else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else{f=b^c^d;k=0xCA62C1D6;}
        t=ROL32(a,5)+f+e+k+w[i]; e=d; d=c; c=ROL32(b,30); b=a; a=t;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;
}
static void sha1_update(sha1_t *s, const void *data, int len) {
    const unsigned char *p = (const unsigned char*)data;
    s->len += (uint64_t)len;
    while(len>0){
        int n = 64-s->buflen < len ? 64-s->buflen : len;
        memcpy(s->buf+s->buflen, p, n); s->buflen+=n; p+=n; len-=n;
        if(s->buflen==64){ sha1_block(s,s->buf); s->buflen=0; }
    }
}
static void sha1_final(sha1_t *s, unsigned char out[20]) {
    uint64_t bits = s->len*8;
    unsigned char pad=0x80; sha1_update(s,&pad,1);
    while(s->buflen!=56){ pad=0; sha1_update(s,&pad,1); }
    for(int i=7;i>=0;i--){ unsigned char b=(unsigned char)(bits>>(i*8)); sha1_update(s,&b,1); }
    for(int i=0;i<5;i++){ out[i*4]=(unsigned char)(s->h[i]>>24); out[i*4+1]=(unsigned char)(s->h[i]>>16); out[i*4+2]=(unsigned char)(s->h[i]>>8); out[i*4+3]=(unsigned char)(s->h[i]); }
}

/* ── Recv helpers ────────────────────────────────────────────────────────────── */
static int ws_recv_all(ws_sock_t s, char *buf, int n) {
    int got=0;
    while(got<n){ int r=recv(s,buf+got,n-got,0); if(r<=0)return -1; got+=r; }
    return got;
}
static int ws_send_all(ws_sock_t s, const char *buf, int n) {
    int sent=0;
    while(sent<n){ int r=send(s,buf+sent,n-sent,0); if(r<=0)return -1; sent+=r; }
    return sent;
}

/* ── Server: accept WebSocket upgrade ────────────────────────────────────────── */
/* Returns 0 on success, -1 on failure. Reads the HTTP upgrade request. */
static int ws_server_handshake(ws_sock_t s) {
    char req[4096]; int got=0;
    /* Read until \r\n\r\n */
    while(got < (int)sizeof(req)-1) {
        int n=recv(s, req+got, 1, 0);
        if(n<=0) return -1;
        got++;
        req[got]='\0';
        if(got>=4 && memcmp(req+got-4,"\r\n\r\n",4)==0) break;
    }
    /* Find Sec-WebSocket-Key */
    char *key_hdr = strstr(req, "Sec-WebSocket-Key:");
    if(!key_hdr) return -1;
    key_hdr += 18;
    while(*key_hdr==' ') key_hdr++;
    char key[64]={0}; int ki=0;
    while(*key_hdr && *key_hdr!='\r' && *key_hdr!='\n' && ki<63) key[ki++]=*key_hdr++;
    key[ki]='\0';
    /* Compute accept: SHA1(key + magic), base64 */
    char combined[128];
    snprintf(combined, sizeof(combined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1_t sh; sha1_init(&sh);
    sha1_update(&sh, combined, (int)strlen(combined));
    unsigned char digest[20]; sha1_final(&sh, digest);
    char accept[64]; b64_encode(digest, 20, accept);
    /* Send 101 */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    return ws_send_all(s, resp, rlen) == rlen ? 0 : -1;
}

/* ── Client: send WebSocket upgrade ─────────────────────────────────────────── */
static int ws_client_handshake(ws_sock_t s, const char *host, const char *path) {
    /* Send a minimal valid upgrade request */
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path, host);
    if(ws_send_all(s, req, rlen) != rlen) return -1;
    /* Read response — just check for 101 */
    char resp[1024]; int got=0;
    while(got<(int)sizeof(resp)-1){
        int n=recv(s,resp+got,1,0); if(n<=0)return -1;
        got++; resp[got]='\0';
        if(got>=4 && memcmp(resp+got-4,"\r\n\r\n",4)==0) break;
    }
    return strstr(resp,"101")!=NULL ? 0 : -1;
}

/* ── Frame send (server: no mask; client: masked) ────────────────────────────── */
static int ws_send_frame(ws_sock_t s, const char *data, int len, int client_side) {
    unsigned char hdr[14]; int hlen=2;
    hdr[0]=0x82; /* FIN + binary */
    uint32_t mask_key=0;
    if(client_side){
        hdr[1]=0x80; /* mask bit */
        /* simple mask */
        mask_key=0x37FA213D;
    } else hdr[1]=0;
    if(len<=125)      { hdr[1]|=(unsigned char)len; }
    else if(len<65536){ hdr[1]|=126; hdr[hlen++]=(unsigned char)(len>>8); hdr[hlen++]=(unsigned char)len; }
    else              { hdr[1]|=127; for(int i=7;i>=0;i--) hdr[hlen++]=(unsigned char)((uint64_t)len>>(i*8)); }
    if(client_side){ hdr[hlen++]=(mask_key>>24)&0xFF; hdr[hlen++]=(mask_key>>16)&0xFF; hdr[hlen++]=(mask_key>>8)&0xFF; hdr[hlen++]=mask_key&0xFF; }
    if(ws_send_all(s,(char*)hdr,hlen)!=hlen) return -1;
    if(client_side){
        unsigned char *tmp=(unsigned char*)malloc(len);
        if(!tmp)return -1;
        unsigned char mk[4]={(mask_key>>24)&0xFF,(mask_key>>16)&0xFF,(mask_key>>8)&0xFF,mask_key&0xFF};
        for(int i=0;i<len;i++) tmp[i]=(unsigned char)data[i]^mk[i%4];
        int r=ws_send_all(s,(char*)tmp,len); free(tmp); return r==len?0:-1;
    }
    return ws_send_all(s,data,len)==len?0:-1;
}

/* ── Frame recv → returns malloc'd payload, sets *out_len. Caller frees. ─────── */
static char *ws_recv_frame(ws_sock_t s, int *out_len) {
    unsigned char hdr[2];
    if(ws_recv_all(s,(char*)hdr,2)<0) return NULL;
    /* hdr[0]: FIN+opcode. opcode 8=close,9=ping,0xA=pong */
    int opcode = hdr[0]&0x0F;
    if(opcode==8) return NULL; /* close */
    int masked = (hdr[1]&0x80)!=0;
    uint64_t plen = hdr[1]&0x7F;
    if(plen==126){ unsigned char x[2]; if(ws_recv_all(s,(char*)x,2)<0)return NULL; plen=(x[0]<<8)|x[1]; }
    else if(plen==127){ unsigned char x[8]; if(ws_recv_all(s,(char*)x,8)<0)return NULL; plen=0; for(int i=0;i<8;i++) plen=(plen<<8)|x[i]; }
    if(plen>1048576) return NULL;
    unsigned char mk[4]={0};
    if(masked && ws_recv_all(s,(char*)mk,4)<0) return NULL;
    char *buf=(char*)malloc((size_t)plen+1);
    if(!buf)return NULL;
    if(ws_recv_all(s,buf,(int)plen)<0){free(buf);return NULL;}
    if(masked) for(uint64_t i=0;i<plen;i++) buf[i]^=mk[i%4];
    buf[plen]='\0';
    if(out_len) *out_len=(int)plen;
    /* handle ping */
    if(opcode==9){ ws_send_all(s,"\x8A\x00",2); return ws_recv_frame(s,out_len); }
    return buf;
}

/* ── High-level: send our length-prefixed message over WebSocket ─────────────── */
static int ws_send_msg(ws_sock_t s, const char *payload, int client_side) {
    /* Our protocol: [4-byte big-endian len][payload] — wrap as one WS frame */
    int plen = (int)strlen(payload);
    int total = 4 + plen;
    char *buf = (char*)malloc(total);
    if(!buf) return -1;
    uint32_t wlen = (uint32_t)plen;
    buf[0]=(wlen>>24)&0xFF; buf[1]=(wlen>>16)&0xFF; buf[2]=(wlen>>8)&0xFF; buf[3]=wlen&0xFF;
    memcpy(buf+4, payload, plen);
    int r = ws_send_frame(s, buf, total, client_side);
    free(buf);
    return r;
}

/* Recv our protocol message from a WebSocket frame */
static char *ws_recv_msg(ws_sock_t s) {
    int flen;
    char *frame = ws_recv_frame(s, &flen);
    if(!frame || flen < 4) { free(frame); return NULL; }
    /* First 4 bytes = our length prefix */
    uint32_t plen = ((unsigned char)frame[0]<<24)|((unsigned char)frame[1]<<16)|((unsigned char)frame[2]<<8)|(unsigned char)frame[3];
    if((int)plen != flen-4) { free(frame); return NULL; }
    /* Return just the JSON payload */
    char *msg = (char*)malloc(plen+1);
    if(!msg){ free(frame); return NULL; }
    memcpy(msg, frame+4, plen);
    msg[plen]='\0';
    free(frame);
    return msg;
}

#endif /* WS_H */
