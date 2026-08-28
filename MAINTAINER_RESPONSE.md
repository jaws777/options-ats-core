# Maintainer Incident Response

This runbook defines how maintainers handle Code of Conduct and security-adjacent
community incidents in a consistent, auditable, and non-retaliatory way.

## Roles

- Enforcement owner: repository maintainers listed in `CODEOWNERS`.
- Primary intake channels: `conduct@jaworski.dev` and GitHub private advisories.

## Triage SLA

- Acknowledge receipt within 2 business days.
- Initial risk assessment within 5 business days.
- Resolution target within 15 business days, unless external/legal constraints
  require more time.

## Process

1. Intake: record time, channel, summary, and available evidence.
2. Safety triage: determine immediate risk and temporary protections.
3. Investigation: collect evidence from private channels and repository history.
4. Decision: apply proportional action and document rationale.
5. Closeout: notify reporter and respondent of outcome scope.

## Temporary moderation actions

Actions may include temporary comment lock, temporary contributor mute,
conversation pause, or reassignment of review ownership.

## Recusal and conflict handling

If a maintainer is directly involved in an incident, that maintainer must recuse
from investigation and decision-making for that case.

## Appeal path

A respondent may submit one written appeal within 14 days of decision notice.
Appeals are reviewed by maintainers not involved in the original decision.

## Public communication policy

- Default to sanitized public summaries.
- Do not disclose sensitive personal details.
- Do not publish secrets, exploit details, or identifying reporter metadata.
- Do not publicly shame contributors.
