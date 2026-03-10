#!/usr/bin/env python3
"""
Fingerprint driver acceptance test.
Tests correct finger acceptance and wrong finger rejection.
"""

import subprocess
import sys
import time

USER = subprocess.check_output(["whoami"]).decode().strip()
FINGER = "right-index-finger"


def pause(msg):
    input(f"\n>>> {msg}\n    Press Enter when ready... ")


def run_verify():
    result = subprocess.run(
        ["fprintd-verify", "-f", FINGER, USER],
        capture_output=True, text=True
    )
    output = result.stdout + result.stderr
    for line in output.splitlines():
        if "Verify result" in line:
            matched = "verify-match" in line and "no-match" not in line
            print(f"    {line.strip()}  {'✓' if matched else '✗'}")
            return matched
    print(f"    (no result line found)")
    return False


def run_enroll():
    print(f"  Enrolling {FINGER} for {USER}...")
    subprocess.run(["fprintd-delete", USER], capture_output=True)
    result = subprocess.run(
        ["fprintd-enroll", "-f", FINGER, USER],
        capture_output=True, text=True
    )
    if "enroll-completed" in result.stdout + result.stderr:
        print("  Enrollment complete ✓")
        return True
    else:
        print("  Enrollment FAILED ✗")
        print(result.stdout + result.stderr)
        return False


# ── Setup ────────────────────────────────────────────────────────────────────

print("=" * 60)
print("  EgisTec EH576 fingerprint driver acceptance test")
print("=" * 60)

pause("Step 1: Enroll your RIGHT INDEX finger.\n"
      "    Place it firmly when prompted.")

if not run_enroll():
    sys.exit(1)

# ── Correct finger ───────────────────────────────────────────────────────────

print("\n── Correct finger (right index) ──────────────────────────")
correct_results = []
for i in range(1, 6):
    pause(f"Step 2.{i}: Place your RIGHT INDEX finger (attempt {i}/5).")
    correct_results.append(run_verify())
    time.sleep(0.5)

correct_pass = sum(correct_results)
print(f"\n  Correct finger: {correct_pass}/5 matched")

# ── Wrong finger ─────────────────────────────────────────────────────────────

print("\n── Wrong finger (right middle) ───────────────────────────")
middle_results = []
for i in range(1, 4):
    pause(f"Step 3.{i}: Place your RIGHT MIDDLE finger (attempt {i}/3).")
    middle_results.append(run_verify())
    time.sleep(0.5)

middle_pass = sum(middle_results)
print(f"\n  Middle finger: {middle_pass}/3 matched (should be 0)")

# ── Other wrong finger ───────────────────────────────────────────────────────

print("\n── Wrong finger (any other) ──────────────────────────────")
other_results = []
for i in range(1, 4):
    pause(f"Step 4.{i}: Place a DIFFERENT finger — thumb, left hand, etc. (attempt {i}/3).")
    other_results.append(run_verify())
    time.sleep(0.5)

other_pass = sum(other_results)
print(f"\n  Other finger: {other_pass}/3 matched (should be 0)")

# ── Summary ──────────────────────────────────────────────────────────────────

print("\n" + "=" * 60)
print("  RESULTS SUMMARY")
print("=" * 60)
print(f"  Correct finger (right index):  {correct_pass}/5 accepted  {'✓ PASS' if correct_pass >= 4 else '✗ FAIL'}")
print(f"  Wrong finger  (right middle):  {middle_pass}/3 accepted  {'✓ PASS' if middle_pass == 0 else '✗ FAIL (false positives)'}")
print(f"  Wrong finger  (other):         {other_pass}/3 accepted  {'✓ PASS' if other_pass == 0 else '✗ FAIL (false positives)'}")

overall = correct_pass >= 4 and middle_pass == 0 and other_pass == 0
print(f"\n  Overall: {'✓ PASS' if overall else '✗ FAIL'}")
print("=" * 60)
