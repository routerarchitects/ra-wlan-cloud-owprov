#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== 1. Checking Microservice Health ==="
if ! curl -s -k https://localhost:16001/api/v1/system?command=info > /dev/null 2>&1; then
    echo "Microservices not reachable. Attempting to spin up containers..."
    
    # Locate docker-compose directory
    DOCKER_DIR=""
    if [ -f "/home/iotina/openwifi-sdk/mango-cloud-deployment/docker-compose/docker-compose.yml" ]; then
        DOCKER_DIR="/home/iotina/openwifi-sdk/mango-cloud-deployment/docker-compose"
    elif [ -f "$SCRIPT_DIR/../../docker-compose.yml" ]; then
        DOCKER_DIR="$SCRIPT_DIR/../.."
    elif [ -f "./docker-compose.yml" ]; then
        DOCKER_DIR="."
    fi

    if [ -n "$DOCKER_DIR" ]; then
        echo "Found docker-compose at: $DOCKER_DIR"
        (cd "$DOCKER_DIR" && docker compose up -d) || true
    else
        echo "Warning: docker-compose.yml not found. Proceeding with test execution..."
    fi

    echo "Waiting for OWSEC & OWProv to be ready..."
    for i in {1..30}; do
        if curl -s -k https://localhost:16001/api/v1/system?command=info > /dev/null 2>&1; then
            echo "OWSEC is Ready! Warming up microservice inter-service connections for 10 seconds..."
            sleep 10
            break
        fi
        echo "Waiting for OWSEC (attempt $i/30)..."
        sleep 2
    done
else
    echo "OWSEC microservice is already up and healthy!"
fi

echo "=== 2. Executing RBAC Data-Driven Lab Test Suite ==="
cd "$SCRIPT_DIR"
go run main.go
