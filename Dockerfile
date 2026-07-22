# =============================================================================
# Dockerfile - Mass Relay Server
# Deployed to Render.com as a Web Service (TCP)
# =============================================================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc libc6-dev make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY common/ ./common/
COPY relay/msrelay.c ./relay/msrelay.c

RUN gcc relay/msrelay.c \
    -I./common \
    -O2 -Wall -Wno-stringop-truncation \
    -D_GNU_SOURCE \
    -lpthread \
    -o msrelay

# ── Runtime image (minimal) ───────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN useradd -r -s /bin/false msrelay

WORKDIR /app
COPY --from=builder /build/msrelay .
RUN chown msrelay:msrelay /app/msrelay && chmod 500 /app/msrelay

USER msrelay

# Render sets PORT env var. We read it at runtime (see entrypoint).
# Agent port  = $PORT      (Render exposes this)
# Manager port = $PORT + 1 (internal, agents+managers must use same base port logic)
# NOTE: Render free tier only exposes ONE port via $PORT.
# We handle this by using a single port with a role field in the auth message
# (already in our protocol: role="agent" vs role="manager").
# The relay's main() is updated to use single-port dispatch when RENDER=1.

EXPOSE 7744

ENV RELAY_TOKEN=""
ENV PORT=7744

ENTRYPOINT ["/bin/sh", "-c", "exec /app/msrelay --port ${PORT} --token ${RELAY_TOKEN}"]
