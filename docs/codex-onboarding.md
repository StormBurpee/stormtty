# Codex onboarding contract

Opening the Windows Codex panel can discover an installed native CLI and launch
its local `app-server` for status. This is not a model task. Startup, configuration
load and rendering alone do not launch it. An optional Advanced executable
override still requires the explicit Connect confirmation because it is not an
automatically recognized installation. No CLI is installed automatically.

Normal setup asks for neither a path nor an account identifier. The service sends
`initialize`, `initialized`, then `account/read` with `refreshToken:false`.
Account identity comes from the provider response, never `auth.json`, tokens,
passwords, clipboard contents or an email entered into StormTTY. The currently
supported verified identity is a ChatGPT email. Other account types or accounts
without email remain blocked with an explanation; API-key entry is not offered.

Sign in is an explicit action. It sends only stable
`account/login/start {type:"chatgpt"}`. The provider owns OAuth, callback handling
and credential storage. StormTTY retains the returned login ID and URL only in
memory. Continue in browser is a separate user action. The native host must
check provider/settings generations, login ID and `valid_agent_auth_url()` before
opening it. Only exact HTTPS origins `auth.openai.com` and `chatgpt.com` are allowed;
ports, userinfo, suffix-host tricks, controls and backslashes are rejected. The
URL, protocol auth error payloads and provider stderr are never logged or saved.

Cancel immediately retires the local browser affordance and sends the provider's
`account/login/cancel` by login ID. If the start response has not arrived, it is
cancelled when it arrives; retries wait for that response (Disconnect is available
if the provider hangs). Old completion IDs and cancellation acknowledgements
cannot satisfy a newer attempt. A successful completion triggers another
`account/read`; it does not start a thread. Account updates clear saved thread
associations, approvals and transcript before verifying the new identity. A
changed account during active work fails closed and requires reconciliation.

Every `thread/start`, `thread/resume` and `turn/start` remains behind the separate
explicit Start confirmation. Neither checking status nor login emits those
methods. Account email is the stable API identity available here, not proof of a
particular organization; automatic organization-specific resume is not promised.

## Verified references and limits

- [Official account and authentication APIs](https://learn.chatgpt.com/docs/app-server#authentication-endpoints)
  checked 2026-10-29: `account/read`, managed browser login, login-completed and
  account-updated notifications, and login cancellation. Baseline remains stable
  app-server v2 (repository baseline rust-v0.165.0); no experimental auth tokens.
- [Discovery layouts and supported install roots](codex-discovery.md).

Tests use authored protocol and filesystem fixtures, including unsigned-in,
signed-in, failed/cancelled login, late completion, invalid URL, identity changes,
missing installation and server disconnect. They do not launch real Codex or
open a real browser, and do not prove current native OAuth/provider runtime
compatibility. Native visual/UI verification is recorded separately. Opening an
app-server may perform provider-managed initialization/network work; the claim is
**no model task is started**, not “no process/network activity.”
