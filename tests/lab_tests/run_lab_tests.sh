#!/usr/bin/env bash
set -e

echo "=== 1. Starting Ephemeral Microservice Containers (PostgreSQL, OWSEC, OWProv) ==="
docker compose up -d

echo "=== 2. Waiting for Microservices to become Healthy & Ready ==="
for i in {1..30}; do
    if curl -s -k https://localhost:16001/api/v1/system?command=info > /dev/null 2>&1; then
        echo "OWSEC is Ready!"
        break
    fi
    echo "Waiting for OWSEC (attempt $i/30)..."
    sleep 2
done

echo "=== 3. Executing RBAC Data-Driven Lab Test Suite ==="
cd "$(dirname "$0")"
go run main.go
