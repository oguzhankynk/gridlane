# System interface description of gridlane_control 1.0.0

Model hash: da61da3a9fbe26899017b3e23413171eee6c86490cb889f1d43742d1a2acbf84

## Units

| Unit | Description | Language |
|---|---|---|
| coordinator | Starts the job, drives the phases, collects results. | per command |
| worker | One application; runs one or more worker threads. | per command |

## Interfaces and units

| Interface | Version | coordinator | worker |
|---|---|---|---|
| control | 1.0.0 | sends, receives | sends, receives |

## Messages

| Message | Interface | Id | From | To |
|---|---|---|---|---|
| Heartbeat | control | 0x40 | coordinator | worker |
| HeartbeatAck | control | 0x41 | worker | coordinator |
| JobStart | control | 0x10 | coordinator | worker |
| PhaseDone | control | 0x21 | worker | coordinator |
| PhaseStart | control | 0x20 | coordinator | worker |
| Progress | control | 0x30 | worker | coordinator |
| ResultSummary | control | 0x50 | worker | coordinator |
| Shutdown | control | 0x7f | coordinator | worker |
| VersionQuery | control | 0x1 | coordinator | worker |
| VersionReply | control | 0x2 | worker | coordinator |
