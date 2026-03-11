#!/bin/bash
# Quick enroll+verify smoke test with pauses between steps
set -e

echo "============================================"
echo "  EH576 Quick Enroll + Verify Test"
echo "============================================"

echo ""
echo "Step 1: Deleting old enrollment..."
fprintd-delete "$USER" 2>/dev/null || true
echo "  Done."

echo ""
read -p "Step 2: Place your RIGHT INDEX finger, then press Enter to start enrollment... "
fprintd-enroll -f right-index-finger "$USER"

echo ""
echo "Step 3: Verify (3 attempts)"
for i in 1 2 3; do
    echo ""
    read -p "  Attempt $i/3: Place your RIGHT INDEX finger, then press Enter... "
    fprintd-verify -f right-index-finger "$USER"
done

echo ""
echo "============================================"
echo "  Test complete"
echo "============================================"
