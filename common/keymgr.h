/*
 * keymgr.h - SSH authorized key management helpers
 *
 * Enforces: ONLY the owner's pre-registered public key can SSH in.
 * Every other key, password, keyboard-interactive — all rejected.
 *
 * How it works:
 *   1. During install, owner runs install.ps1 which calls Set-AuthorizedKey
 *      to write their public key to the restricted authorized_keys file.
 *   2. sshd_config is locked to: PubkeyAuthentication yes, everything else NO.
 *   3. The agent service periodically audits the authorized_keys file and
 *      sshd_config to ensure they have not been tampered with.
 *   4. The agent stores a SHA-256 hash of the approved key in the registry.
 *      If the file changes to a different key, the agent stops sshd and alerts.
 */

#ifndef KEYMGR_H
#define KEYMGR_H

#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

#define OWNER_KEYS_FILE   "C:\\ProgramData\\ssh\\administrators_authorized_keys"
#define SSHD_CONFIG_FILE  "C:\\ProgramData\\ssh\\sshd_config"
#define KEY_HASH_REG      "SOFTWARE\\KiroAccess"
#define KEY_HASH_REGVAL   "OwnerKeyHash"

/* ── SHA-256 of a string using Windows CNG ────────────────────────────────── */
static int sha256_hex(const char *input, char *out_hex64) {
    HCRYPTPROV  prov   = 0;
    HCRYPTHASH  hash   = 0;
    BYTE        digest[32];
    DWORD       dlen   = sizeof(digest);
    int         ok     = 0;

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES,
                               CRYPT_VERIFYCONTEXT)) return 0;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) goto done;
    if (!CryptHashData(hash, (BYTE*)input, (DWORD)strlen(input), 0)) goto done;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) goto done;
    for (int i = 0; i < 32; i++)
        sprintf(out_hex64 + i*2, "%02x", digest[i]);
    out_hex64[64] = '\0';
    ok = 1;
done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    return ok;
}

/* ── Read the entire authorized_keys file ─────────────────────────────────── */
static char *read_auth_keys_file(void) {
    FILE *f = fopen(OWNER_KEYS_FILE, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 65536) { fclose(f); return NULL; }
    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ── Save/load approved key hash in registry ──────────────────────────────── */
static void keymgr_save_hash(const char *hex64) {
    HKEY hk;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, KEY_HASH_REG, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, KEY_HASH_REGVAL, 0, REG_SZ,
                       (LPBYTE)hex64, (DWORD)strlen(hex64)+1);
        RegCloseKey(hk);
    }
}

static int keymgr_load_hash(char *out_hex64) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, KEY_HASH_REG, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    DWORD sz = 65;
    int ok = RegQueryValueExA(hk, KEY_HASH_REGVAL, NULL, NULL,
                               (LPBYTE)out_hex64, &sz) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return ok;
}

/*
 * keymgr_audit - Call this periodically from the agent.
 *
 * Returns:
 *   1  - everything OK
 *   0  - tampering detected (agent should stop sshd and log alert)
 *  -1  - file missing or unreadable (sshd should be stopped)
 */
static int keymgr_audit(void) {
    char *keys = read_auth_keys_file();
    if (!keys) return -1;

    char current_hash[65] = "";
    sha256_hex(keys, current_hash);
    free(keys);

    char saved_hash[65] = "";
    if (!keymgr_load_hash(saved_hash)) {
        /* First run after install — save it as baseline */
        keymgr_save_hash(current_hash);
        return 1;
    }

    if (strcmp(current_hash, saved_hash) != 0) {
        /* File was changed after install — potential unauthorised key injection */
        return 0;
    }
    return 1;
}

/*
 * keymgr_register_key - Called from installer/initial setup.
 * Writes the owner's public key to authorized_keys and saves the hash baseline.
 */
static int keymgr_register_key(const char *pubkey_line) {
    /* Create directory if needed */
    CreateDirectoryA("C:\\ProgramData\\ssh", NULL);

    FILE *f = fopen(OWNER_KEYS_FILE, "w");
    if (!f) return 0;
    /* Write exactly one key, trimmed, newline terminated */
    fprintf(f, "%s\n", pubkey_line);
    fclose(f);

    /* Lock down file permissions: SYSTEM + Administrators only */
    /* Using icacls via ShellExecute — simplest cross-version approach */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "icacls \"%s\" /inheritance:r /grant SYSTEM:F /grant Administrators:F",
        OWNER_KEYS_FILE);
    system(cmd);

    /* Save hash baseline */
    char hash[65] = "";
    sha256_hex(pubkey_line, hash);
    keymgr_save_hash(hash);
    return 1;
}

#endif /* KEYMGR_H */
