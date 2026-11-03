# Supported target

This beta is deliberately locked to one firmware image:

| Field | Value |
|---|---|
| Train | `MOI3_EU_SE_R8410L` |
| Market | NAR content (despite the `EU` train label) |
| MU | `0841` |
| Part number | `575919360B` |
| CPU package | `MIB3OI_CPU` `1.841.3` |
| HMI package | `Large_SEAT_EU` `1.840.0` |
| CarPlay plug-in | `320.17.4` |
| CarPlay binary SHA-256 | `6d32486f5ec302c55ccf73b9884e20b1cf6d2773174abfe56245b2d3c7f4ce16` |

The installer also checks the stock AirPlay/CoreUtils libraries. A mismatch is
an intentional hard stop, not an error to bypass.

## Static compatibility result

The target is AArch64 and uses glibc 2.17. Its dynamic symbol tables contain
the seven interposition points used by the handler:

- `AirPlayCopyServerInfo`
- `AirPlayReceiverSessionSetup`
- `AirPlayReceiverSessionTearDown`
- `AES_CBCFrame_Init`
- `ScreenStreamCreate`
- `ScreenStreamStop`
- `ScreenStreamSetWidthHeight`

The stock cluster service launches `multi-screen-server`; the firmware's
display-management service and `navi-kombi` pipeline use the expected MOI3 LG
architecture. This is strong static evidence, but it is not a substitute for
an in-car test.
