# Security Triage and Secret Remediation Notes

This document captures policy for handling scanner findings.

## Workflow

1. Run dual scans (`gitleaks` and `trufflehog`) on working tree and git history.
2. Classify findings as true positive, false positive, or acceptable test data.
3. For true positives:
   - rotate/revoke exposed credentials
   - remove/rewrite sensitive material where appropriate
   - document remediation timestamp and owner in private records
4. Re-run both scanners and require zero untriaged findings.

## Public reporting rules

- Do not publish raw secret values.
- Do not publicly attribute faults by individual contributor by default.
- Publish sanitized remediation summaries only.
