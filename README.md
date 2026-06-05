# QCON — NSDI'26 Artifact

**QCON** is a QoE-aware multi-connectivity scheduler for mobile
real-time video. It runs in the RAN, infers per-application QoE, and
schedules across LTE + 5G NR links to use the valuable backup link
minimally — keeping radio resource utilization efficient while still
meeting application QoE.

## Abstract

Mobile real-time video streaming (RTS) applications — cloud gaming
and AR/VR — require consistent high throughput and low latency to
satisfy user Quality of Experience (QoE), yet today's wireless links
fluctuate wildly. While multi-path solutions seem promising to tackle
such single-link fluctuations, existing transport-level solutions
require multiple cellular subscriptions, which most users don't have.
In this paper, we leverage 5G multi-connectivity, which allows
simultaneous connection to multiple base stations (e.g., 5G and 4G)
and is already deployed in commercial networks. However, our
measurements show RTS applications still suffer from single-link
fluctuations due to operators' deliberate policies restricting
multi-connectivity to conserve 4G backup links regardless of
application demands. To optimize application QoE while respecting
operator policies, we present QCON, a QoE-driven multi-connectivity
solution that efficiently utilizes backup links based on precise
application QoE. For practical deployment, we design a QoE inference
module that operates within the RAN and develop multi-link scheduling
to optimize both QoE and radio resource efficiency. We also design
priority-based re-injection utilizing RAN link recovery mechanism to
prevent video stalls. Our prototype implementation of QCON on a RAN
intelligent controller within an Open-RAN testbed demonstrates 2.1×
improvements of bitrates, enhancing tail frame rates by 4–5× with
efficient backup link use compared to existing multi-link scheduling
schemes.

This repository contains:

| Dir | What |
|---|---|
| `emulator/` | User-space PDCP + bandwidth emulator. No SDR required. |
| `ric/` | C++ RIC controller (QoE-aware multi-connectivity scheduler) + Python protocol prototype. |
| `testbed/` | OAI fork (eNB + gNB) + Magma EPC compose for real-radio runs. |

Each subdir has its own `README.md` for build/run.

## 🚧 What's Next

We are currently building a **RIC platform** that makes app-insights
easily accessible to the RAN. QCON will be integrated into this
platform.

**Stay tuned!** ⭐ Star this repo to get notified.

## License

MIT.
