# M155 source review

**Reviewed on 2026-10-24.** The known SR-003 findings and the October Chrome desktop WebRTC advisories are accounted for in the selected ARM64 backend. No dependency pin or native implementation changed during this review.

This closes the source/advisory investigation for these inputs. Independent-host builds and physical iOS acceptance remain under SR-006/SR-007. It does not establish network behavior or approve a release; `Connect` is still unimplemented, and final distribution notices remain SR-902 work.

## What is in the library

The audit reads the actual archive inputs, checks the compiled archive against its build record, verifies source revisions and local patches, and hashes each compiled source file. It covers Mac, Android, iOS device and iOS simulator artifacts.

| Code | Source identity |
|---|---|
| WebRTC, including SCTP and small audio helpers | Pinned WebRTC commit and per-file hashes |
| BoringSSL, VPX, SRTP, YUV and Perfetto | Separate pinned repositories |
| Opus, Abseil, PFFFT and RNNoise | Pinned Chromium `third_party` tree, with upstream revision metadata |
| JPEG conversion | Included on Mac and Android; absent from the Apple mobile archives |
| CPU feature detection | Included on Android, from the pinned `third_party` tree |

The inventory contains 2,608 object inputs on Mac, 2,594 on Android and 2,492 on each Apple mobile target. These are archive inputs, not claims that every function survives final app linking. Android's additional compiler helpers retain their NDK archive hash in the artifact's build/link records.

AV1/libaom, dav1d and Blink are absent from the compiled inputs. Protobuf tools used to generate tracing code are build tools, not compiled Protobuf runtime objects in this archive. The backend uses its own crypto and Opus code; Unreal's unrelated engine libraries remain engine-owned.

## Findings and resolutions

| Finding | Result |
|---|---|
| Opaque Epic WebRTC/crypto/codec inputs | Replaced in the RTC backend by the pinned source builds |
| CVE-2023-7024 | The upstream fix concerns Blink's audio sink, which is absent here |
| CVE-2024-5493 | The upstream fix concerns libaom; AV1/libaom is disabled |
| CVE-2026-9111 / 9119 / 9120 | Fix commits are ancestors of our WebRTC pin; the relevant source changes remain present. PipeWire is also excluded |
| CVE-2026-103631 / 106268 / 106357 | The M155 packetizer and transceiver fixes are included |
| CVE-2026-110137 / 113161 / 113303 | Upstream fixes affect Blink lifetime handling, which is not part of these native builds |
| VPX CVE-2024-5197 / CVE-2026-1861 | The selected VPX history includes both fixes; allocation and output-buffer checks remain present |
| Simulator nghttp2 CVE-2026-27135 | Previously fixed and validated with the pinned 1.70.0 support library |

The [review snapshot](reviews/SR008-M155-source-review.json) retains the exact fix commits, upstream review links, excluded paths, source hashes and advisory queries. Git ancestry checks use a separate review checkout and do not alter build sources. Local patches only adjust build selection and toolchain integration; they do not replace the upstream crypto or codec algorithms.

The review includes Google's [May WebRTC findings](https://chromereleases.googleblog.com/2026/05/stable-channel-update-for-desktop_0841193308.html), [October 6 fixes](https://chromereleases.googleblog.com/2026/10/stable-channel-update-for-desktop_086471744.html) and [October 20 fixes](https://chromereleases.googleblog.com/2026/10/stable-channel-update-for-desktop_02018666648.html). Each issue was mapped to its source changes rather than inferred from a Chrome version number.

The recorded OSV commit queries returned no findings. The queried upstream GitHub advisory feeds were empty too. These results do not prove complete database coverage or absence of defects. Refresh the review when inputs change and before accepting untrusted network traffic or publishing a release.

## Repeat the source inventory

Use the matching source workspace and artifact. The tool only reads them:

```sh
python3 tools/audit_native_sources.py \
  --work LocalBuilds/NativeBackend \
  --artifact LocalBuilds/Artifacts/M155-Mac \
  --output Intermediate/M155-Mac-sources.json
```

For Android, run the same command inside its pinned builder with the matching Linux build workspace. Source inventories are separate from [repeat-build comparisons](NATIVE_BACKEND.md#check-repeat-builds), runtime tests and final app notices.
