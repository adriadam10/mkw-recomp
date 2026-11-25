<!-- translated from README.md 7b79005459a3 body d1063a4c7816 -->

<p align="center"><img src="../images/logo.svg" width="128" alt="mkw-recomp logo"></p>

<h1 align="center">mkw-recomp</h1>

<p align="center">
  <b>Mario Kart World (Nintendo Switch 2) được biên dịch lại để chạy nguyên bản trên Linux và Windows.</b>
</p>

<p align="center">
  <a href="https://github.com/adriadam10/mkw-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_T%E1%BA%A3i_xu%E1%BB%91ng-b%E1%BA%A3n_m%E1%BB%9Bi_nh%E1%BA%A5t_cho_Windows-0078d6?style=for-the-badge&labelColor=1b2740" alt="Tải xuống: bản mới nhất cho Windows"></a>
  <a href="https://github.com/adriadam10/mkw-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_T%E1%BA%A3i_xu%E1%BB%91ng-b%E1%BA%A3n_m%E1%BB%9Bi_nh%E1%BA%A5t_cho_Linux_%28AppImage%29-e95420?style=for-the-badge&labelColor=1b2740" alt="Tải xuống: bản mới nhất cho Linux (AppImage)"></a>
  <a href="https://github.com/adriadam10/mkw-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_T%E1%BA%A3i_xu%E1%BB%91ng-b%E1%BA%A3n_m%E1%BB%9Bi_nh%E1%BA%A5t_cho_Linux_ARM-8957e5?style=for-the-badge&labelColor=1b2740" alt="Tải xuống: bản mới nhất cho Linux ARM"></a>
</p>

<p align="center">
  <b><a href="https://github.com/adriadam10/mkw-recomp/releases">Bản phát hành</a></b> ·
  <b><a href="https://github.com/adriadam10/mkw-recomp/wiki">Wiki trợ giúp</a></b> ·
  <b><a href="https://discord.gg/8uYBkjQkEg">Discord</a></b> ·
  <a href="../../CHANGELOG.md">Nhật ký thay đổi</a> ·
  <a href="../../CONTRIBUTING.md">Đóng góp</a> ·
  <a href="../../docs/project/research.md#sources">Nguồn</a> ·
  <a href="../../docs/project/roadmap.md">Lộ trình</a> ·
  <a href="../../docs/project/performance.md">Hiệu năng</a> ·
  <a href="https://github.com/adriadam10/mkw-recomp/issues">Vấn đề</a> ·
  <a href="https://github.com/adriadam10/mkw-recomp/discussions">Thảo luận</a> ·
  <a href="https://archive.org/search?query=subject%3A%22mkw-recomp%22">Lưu trữ</a>
</p>

<!-- i18n-bar:start -->
<p align="center">
  <a href="../../README.md">English</a> ·
  <a href="README.zh-CN.md">简体中文</a> ·
  <a href="README.es.md">Español</a> ·
  <a href="README.ar.md">العربية</a> ·
  <a href="README.hi.md">हिन्दी</a> ·
  <a href="README.pt-BR.md">Português (Brasil)</a> ·
  <a href="README.pt-PT.md">Português (Portugal)</a> ·
  <a href="README.de.md">Deutsch</a> ·
  <b>Tiếng Việt</b>
</p>
<!-- i18n-bar:end -->

*Mario Kart World* (Switch 2, các phiên bản 1.0.0 và 1.8.0) được chuyển thành một chương trình nguyên bản (native) cho
Linux và Windows. Bạn cần bản game của chính mình, được dump từ chính máy console của bạn: không có chút gì của game
trong kho này hay trong các bản phát hành của nó.

**Đây là một bản biên dịch lại (recompilation), không phải giả lập.** Mã AArch64 của game được dịch sang C từ trước và
biên dịch cho CPU của bạn (x86-64, hoặc AArch64 trên Linux); không có trình thông dịch CPU hay JIT nào. GPU cũng không
bị giả lập: NVN2, API đồ họa của Switch 2, chạy trên nền Vulkan, và các shader của game được dịch sang SPIR-V từ trước.

## Trạng thái

Còn sớm, và đang tiến triển nhanh (12/11/2026):

- Khởi động được, chiếu các đoạn phim mở đầu, đi qua các menu và đua được, trên Linux và Windows (x86-64) và trên Linux
  AArch64 (AppImage riêng của nó, đã thử trên một thiết bị cầm tay dùng Adreno 740).
- Màn hình bắt đầu thiết lập game từ bản sao của bạn ngay trên máy của bạn và giữ mọi cài đặt.
- Các cuộc đua chạy ở 60 fps trên các GPU cao cấp và 30 fps trên các GPU tầm trung. Thời gian khung hình đã đo theo
  từng GPU: [docs/project/performance.md](../../docs/project/performance.md).
- Các vấn đề đã biết: một vài menu (chọn chế độ, nhân vật và xe) còn cần làm thêm, bóng của kart thỉnh thoảng hụt một
  khung hình, đôi chỗ âm thanh bị ngắt trong các đoạn phim, và trên một số driver AMD thì GPU có thể bị treo. Không có
  chơi trực tuyến.

## Lộ trình

Các công việc còn mở, theo thứ tự (chi tiết: [docs/project/roadmap.md](../../docs/project/roadmap.md)):

- **60 fps trong các cuộc đua**: việc ghi lại khung hình của game cho Vulkan nằm trên đường tới hạn (critical path),
  nên nó cần ít CPU hơn.
- **Hình ảnh như trên console**: các lỗi shader cuối cùng và bóng của kart.
- **Mọi menu đều được kiểm chứng** với console.
- **Không còn treo GPU**, ít bộ nhớ GPU hơn, nạp không giật, một gói Linux cho các bản phân phối không phải rolling.
- **1.8.0 ngang bằng 1.0.0**: ultrawide, kích thước shadow map, seed cố định
  ([#19](https://github.com/adriadam10/mkw-recomp/issues/19)).

## Chơi game

> **Bị kẹt?** Hãy tìm trong [wiki](https://github.com/adriadam10/mkw-recomp/wiki) trước: cài đặt, các thiết lập, điều
> khiển, [các lỗi đã biết theo dòng log của chúng](https://github.com/adriadam10/mkw-recomp/wiki/Troubleshooting) và
> một [FAQ](https://github.com/adriadam10/mkw-recomp/wiki/FAQ).

![Màn hình bắt đầu khi game đã được thiết lập](../../docs/images/launcher-main.png)

![Trang Game files, nơi phần thiết lập nhận bản dump của người chơi](../../docs/images/launcher-files.png)

1. Dump Mario Kart World 1.0.0 hoặc 1.8.0 từ Switch 2 của bạn (`exefs/` và `romfs/`, đã giải mã; với 1.8.0 là bản cập
   nhật đã gộp vào RomFS của bản gốc), hoặc cùng bản dump đó đóng gói thành một tệp `.nup` hoặc `.nx`.
2. Tải launcher từ [Releases](https://github.com/adriadam10/mkw-recomp/releases): bản `.AppImage` trên Linux
   (`-aarch64` trên ARM; glibc 2.31 trở lên: Debian 11, Ubuntu 20.04, SteamOS và mới hơn) hoặc bản `.zip` trên Windows,
   giải nén vào một thư mục. Cần khoảng 30 GB trống.
3. Chạy nó (trước đó `chmod +x` cho AppImage), mở **Game files**, nếu muốn thì **Choose game folder** (mặc định trên
   Linux: `~/.local/share/mkw-recomp`, hoặc thư mục bạn đã giải nén AppImage vào bằng `--appimage-extract`), chỉ nó
   đến bản dump của bạn và nhấn **Set up**. Lần thiết lập đầu tiên mất một lúc: nó biên dịch toàn bộ game và tải xuống
   `nvdisasm` và `ptxas` của NVIDIA (84 MB trên Linux, 126 MB trên Windows). Sau đó nhấn **Play**.
4. Các bản phát hành mới được chào trên màn hình bắt đầu (**Update to vX**); **Cài đặt → Menu → Kiểm tra cập nhật** sẽ
   tắt điều đó.

Với [RecompLauncher](https://github.com/elripalda/RecompLauncher), hãy dùng **Easy Setup** và chọn tệp `.nup`, `.nx`
đã giải mã của bạn hoặc thư mục chứa `exefs/` và `romfs/`. Nó dựng game trong thư mục `recompdata` của game nằm trong
Recomp Library của bạn và dùng bản sao ở đó để dựng lại sau các bản cập nhật. Hãy để khoảng 30 GB trống trong thư viện
đó. RecompLauncher lo các bản cập nhật cho cài đặt này. Game, các thiết lập và dữ liệu lưu của nó ở yên trong thư mục
đó dù bạn dùng Easy Setup hay thiết lập từ màn hình bắt đầu, trên Windows và trên Linux (nơi launcher giải nén AppImage
vào thư viện).

<a href="https://recomplauncher.com/game/mkw-recomp"><img src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" alt="Get it on RecompLauncher" height="56"></a>

Yêu cầu: CPU x86-64 có AVX2 (hoặc, trên Linux, AArch64 với ARMv8.2: LSE, FP16, RCpc, DotProd, CRC32), một GPU có
Vulkan 1.3 (khuyến nghị `VK_EXT_mesh_shader`), khuyến nghị 16 GB RAM. Trong một container Docker hoặc Podman trên
Linux, hãy cho phép `userfaultfd` trong profile seccomp (mặc định của Docker từ chối nó), nếu không nó sẽ chạy chậm
hơn nhiều lần.

Để thêm máy của bạn vào danh sách hiệu năng, hãy mở một
[báo cáo hiệu năng](https://github.com/adriadam10/mkw-recomp/issues/new?template=performance.yml).

## Các tính năng thêm

Vượt ngoài console, tất cả đều tùy chọn và mặc định tắt (hoặc ở giá trị của console):

- **Hiển thị**: bất kỳ độ phân giải nội bộ nào, 21:9 / 32:9 / 48:9, phóng tỉ lệ AMD FSR 1, HDR, VSync, trình bày kiểu
  mailbox hoặc immediate.
- **Tạo khung hình**: các khung hình được tạo ra giữa các khung của game (30 fps được chiếu ở 60, hoặc 240). Mượt hơn,
  không nhanh hơn; chuyển động nhanh và bản đồ nhỏ (minimap) có thể bị nhòe.
- **Tỉ lệ khung hình**: 20 và 15 fps bên cạnh 60 và 30; chế độ Automatic tự hạ xuống các mức đó khi khung hình mất quá
  lâu, để một GPU chậm vẫn giữ cuộc đua ở tốc độ thật. **Độ trễ thấp** (mặc định bật) bắt đầu mỗi khung hình muộn nhất
  mà GPU cho phép.
- **Đồ họa**: lọc texture và chất lượng texture, khoảng cách chi tiết, chất lượng bóng.
- **Âm thanh**: âm thanh vòm 5.1.
- **Điều khiển**: mọi phím và nút đều gán lại được, camera bằng chuột, các biểu tượng Nintendo, Xbox hoặc PlayStation,
  lái bằng chuyển động, rung, chơi nhiều người tại chỗ với mỗi người một tay cầm. Tay cầm Switch 2 hoạt động qua
  Bluetooth và, trên Linux, qua USB (USB trên Windows: xem bên dưới). Con quay hồi chuyển (gyro) từ một máy chủ
  Cemuhook / DSU (DS4Windows, BetterJoy) được hỗ trợ. Vô lăng đua xe hoạt động như một tay cầm (vô lăng và các nút đầu
  tiên); với bàn đạp, hãy đặt một tệp `gamecontrollerdb.txt` trong thư mục game.
- **Tạm dừng**: F5 tạm dừng và tiếp tục game bất cứ lúc nào, như nút HOME của console (hình ảnh và nhạc dừng lại tại
  chỗ). Khi game đang dùng mạng, như trong một cuộc đua LAN, thay vào đó nó vẫn chạy khi mất tiêu điểm và im lặng, như
  trên console.
- **Hacks**: turbo (F2), bỏ qua các đoạn phim, mở khóa mọi thứ, ngẫu nhiên cố định, đi thẳng vào đua tính giờ hoặc
  chạy tự do.
- **Ngôn ngữ**: các menu bằng 25 ngôn ngữ; game còn có thêm tiếng Catalan, Galicia, Basque, Thổ Nhĩ Kỳ, Việt, Ukraina, Indonesia, Ả Rập và Hindi.
- **Công cụ**: lớp phủ hiệu năng và thông tin hệ thống bằng một cú nhấp cho các báo cáo lỗi.

### Tay cầm Switch 2 qua USB trên Windows

Chúng chỉ khởi động sau một lệnh được gửi tới giao diện USB thứ hai của chúng, giao diện mà Windows để không có driver
khả dụng. Một lần cho mỗi tay cầm, với [Zadig](https://zadig.akeo.ie/):

1. Cắm tay cầm vào và mở Zadig.
2. **Options → List All Devices**, chọn **Interface 1** của tay cầm (USB ID `057E` và `2069` cho Pro Controller,
   `2066`/`2067` cho Joy-Con phải/trái, `2073` cho GameCube). Không phải Interface 0: thay thế nó sẽ giấu tay cầm khỏi
   mọi ứng dụng khác.
3. Driver **WinUSB**, rồi **Replace Driver**.

Điều tương tự áp dụng cho một tay cầm ảo mang các ID đó (chế độ USB/IP của Switch2Connect). Để hoàn tác, gỡ thiết bị
của giao diện đó trong Device Manager với ô "delete the driver" được đánh dấu.

## Mods

Trang **Mods** của màn hình bắt đầu có hai thẻ:

- **Find on GameBanana**: các mod của Mario Kart World trên [GameBanana](https://gamebanana.com/games/22152), kèm tác
  giả, giấy phép, ảnh chụp màn hình và mô tả. **Install** chỉ tải xuống khi bạn yêu cầu.
- **Installed**: mỗi mod có một công tắc bật/tắt; **Import zip / Import folder** nhận một mod bạn đã có (`.zip`, `.rar`
  hoặc `.7z` với `romfs/`, `exefs/` và/hoặc `plugin/` bên trong).

Các mod thay thế tệp của game (`romfs/`) áp dụng ở lần khởi động tiếp theo. Các mod vá mã của game (`exefs/` dạng
`.ips` hoặc `.pchtxt`, như 200cc hay 30 FPS) dựng lại game trước khi bạn chơi (vài phút). Hai mod cùng sửa một tệp:
trang sẽ hỏi cái nào thắng. Nó cũng cảnh báo về các mod thay thế thiết lập đồ họa hoặc shader.

Một mod có thư mục `plugin/` mang theo một plugin nguyên bản (`.dll` hoặc `.so`) chạy chương trình riêng của nó trên
máy tính của bạn: chỉ bật nó nếu bạn tin tưởng nguồn của nó. Với người làm mod, API là một tệp header duy nhất,
[`runtime/plugin/mkw_plugin.h`](../../runtime/plugin/mkw_plugin.h), được giải thích trong trang
[Writing a plugin](https://github.com/adriadam10/mkw-recomp/wiki/Writing-a-plugin) của wiki; thêm nữa ở trang
[Mods](https://github.com/adriadam10/mkw-recomp/wiki/Mods) của wiki.

Mods là do người khác làm và không được kiểm thử ở đây: một mod có thể làm game bị sập hoặc hỏng dữ liệu lưu của bạn.
Hãy giữ một bản sao của dữ liệu lưu, báo các vấn đề của một mod cho tác giả của nó, và khi bạn báo lỗi ở đây, hãy nói
những mod nào đang bật (log có liệt kê chúng).

## Dựng từ mã nguồn

Trên Linux, với `uv`, `clang`, `lld`, `ninja`, SDL3, Opus, libvpx, zstd, xxHash, các header Vulkan và VMA 3.4.0 (chi
tiết trong [tools/build/README.md](../../tools/build/README.md)). Đặt bản dump của bạn vào `game/` (`game/exefs`,
`game/romfs`) hoặc đặt `MKW_GAME=<dir>`, rồi từ thư mục gốc của kho:

```sh
uv run python recomp/gen.py --exefs game/exefs --jobs 8  # --out/--patches default to the dump's version (1.8.0: build/generated-180)
uv run tools/shaders/extract.py && uv run tools/shaders/translate.py
uv run tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw (windeps.py)
build/mkw
```

Các lối tắt: `make build`, `make appimage` (`make help` liệt kê tất cả). Trên chính Windows, từ một bản dump đã đóng
gói, hoặc trên AArch64: xem [tools/build/README.md](../../tools/build/README.md). Việc dịch các shader cần đặc tả mã hóa
SM86, được tạo ra từ `nvdisasm` của NVIDIA (không đi kèm): [tools/shaders/README.md](../../tools/shaders/README.md). Mọi thứ
được tạo ra đều đi vào `build/`, không bao giờ vào git.

| Thư mục | Nội dung |
|---|---|
| [recomp/](../../recomp/README.md) | Bộ sinh mã: AArch64 → C |
| [runtime/](../../runtime/README.md) | Thứ đóng vai console: kernel, bộ nhớ, các thay thế SDK, NVN2 trên Vulkan, âm thanh, đầu vào, các menu và launcher |
| [sm86/](../../sm86/README.md) | Bộ dịch shader: SM86 SASS → GLSL → SPIR-V |
| [i18n/](../../i18n/README.md) | Các văn bản của menu, màn hình bắt đầu và phần thiết lập bằng mọi ngôn ngữ (gettext `.po`) |
| [tools/](../../tools/README.md) | Phân tích, thiết lập, dựng, chạy game, chẩn đoán |
| [tests/](../../tests/README.md) | Các kiểm thử không cần game cũng không cần GPU, chạy trong CI trên Linux và Windows |
| [docs/](../../docs/README.md) | Nghiên cứu, các nguồn, hiệu năng |
| `.github/` | CI, các mẫu issue và pull request |

Ở thư mục gốc: `pyproject.toml` và `uv.lock` (môi trường Python của các công cụ), `.gitignore`, `.gitattributes`
(merge=union cho các danh sách mà mọi PR đều thêm vào), `LICENSE`, `CONTRIBUTING.md`, `CHANGELOG.md`, `AGENTS.md`
(hướng dẫn cho các agent viết code) và `Makefile`.

## Các quyết định thiết kế (30/10/2026)

| Chủ đề | Quyết định | Vì sao |
|---|---|---|
| CPU | Bộ nâng (lifter) AArch64→C dựa trên bộ nâng của [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (MIT), mở rộng tới ARMv8.1-8.3 (các phép nguyên tử LSE, CAS, LDAPR; PAC như identity, các thẻ MTE như 0) | Cùng họ engine, và nó đã mở rộng tới một tệp thực thi lớn |
| Hệ thống | Các hàm `nn::*` của SDK được thay thế theo tên; chữ ký lấy từ các tên đã mangle của 1 289 hàm import của `main` | Không có dịch vụ console nào để giả lập |
| Đồ họa | NVN2 trên Vulkan: 529 trong số 699 hàm của nó trùng tên với NVN1; 170 hàm là mới | Biên dịch lại, không phải giả lập GPU |
| Shaders | Bộ dịch SM86→SPIR-V riêng, chạy ngoại tuyến, với một bộ giải mã được tạo từ đặc tả của DocumentSASS / nvsass-disassembler | Phủ được mesh/task và những gì Maxwell thiếu |
| Các tính năng thêm của Switch 2 | HDR tự động khi màn hình là HDR (Cài đặt > Màn hình > HDR); DLSS, ray tracing và cảm ứng mặc định tắt, các hàm NVN2 của chúng có tồn tại nhưng không làm gì | Các tính năng phần cứng thêm của console, không cần cho việc chơi |
| Mặc định | Vẻ ngoài và độ phân giải của console; các thiết lập thấp hơn là các tùy chọn | Người chơi quyết định |

## Cách nó được làm ra

Được viết với [Claude](https://claude.com/claude-code) (agent viết code của Anthropic) đảm nhiệm phần lớn việc gõ,
dưới sự chỉ đạo của tôi, trong khoảng mười ngày làm việc toàn thời gian (489 commit từ 30/10/2026), mỗi commit được
xem lại, đo đạc hoặc kiểm thử trước khi đưa vào.

- **Phương pháp hơn là prompt.** Các thay đổi về hiệu năng được đo bằng một phép A/B trong cùng một lần chạy (hai lần
  chạy của cùng một bản dựng chênh nhau tới 12 ms mỗi khung hình); các sửa lỗi kết xuất được kiểm chứng đối chiếu với
  chính hình ảnh của console; bộ dịch shader được kiểm thử ngoài GPU trên tất cả 11 561 chương trình; các bản chụp
  khung hình phát lại back end trong vài giây. Các quy tắc sinh ra từ những sai lầm được ghi lại và thực thi bằng
  script.
- **Công việc trước đó.** [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) và bản port Linux của nó,
  các bản recomp trước đây của tôi (SavageWindRecomp, smo-recomp), và việc duy trì
  [AnyPS5](https://github.com/boykopovar/AnyPS5) (dịch shader trung thực, mesh shader dưới dạng compute).
- **Nghiên cứu của cộng đồng.** Các ghi chú về Switch 2 của [eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2), mã
  hóa SASS của NVIDIA như được DocumentSASS và nvsass-disassembler ghi lại, Ryujinx và Mesa NAK cho ngữ nghĩa của
  shader, và các bản port Switch 2 khác có trong các nguồn.

## Giấy phép và ghi công

[Giấy phép MIT](../../LICENSE). Mã lấy từ các dự án khác giữ giấy phép riêng của nó bên cạnh (các tệp `LICENSE.*` trong
[recomp/](../../recomp/), [runtime/](../../runtime/), [sm86/decode/](../../sm86/decode/) và
[runtime/third_party/](../../runtime/third_party/README.md)): totk-pc-recomp, nvsass-disassembler, Dear ImGui, bc7enc,
stb_image, AMD FidelityFX FSR 1 và Roboto. Mọi nguồn, với giấy phép của nó và những gì đã lấy từ đó (mã chỉ từ các dự
án MIT; từ các dự án GPL, chỉ lấy ý tưởng): [docs/project/research.md](../../docs/project/research.md#sources).

Sản phẩm này dùng **NVIDIA DLSS** (Super Resolution và Frame Generation, NVIDIA DLSS SDK, © NVIDIA Corporation),
**AMD FidelityFX Super Resolution 3.1** (© Advanced Micro Devices, MIT) và **RenHDrum** (MIT) cho rung HD. Chúng không
nằm trong gói: phần thiết lập tải từng thứ từ bản phát hành của nhà sản xuất của nó khi bạn chọn nó. NVIDIA, NVIDIA RTX
và DLSS là các thương hiệu của NVIDIA Corporation; dự án này không được NVIDIA hay AMD tài trợ hay chứng thực. Các
thông báo và giấy phép của chúng nằm trong thư mục `licenses/` của gói
([tools/build/THIRD_PARTY_NOTICES.txt](../../tools/build/THIRD_PARTY_NOTICES.txt)).

Mario Kart World là © của Nintendo. Dự án này không liên kết với Nintendo và không được Nintendo chứng thực, không
chứa mã hay tài sản nào của game, và không giúp lấy chúng.

Mods thuộc về các tác giả của chúng và các giấy phép riêng của chúng. Dự án này không liên kết với GameBanana hay tác
giả của bất kỳ mod nào, không lưu trữ, kiểm duyệt hay phân phối mod, và không chịu trách nhiệm về những gì một mod
làm: màn hình bắt đầu chỉ cho xem danh sách công khai của GameBanana và tải một mod từ đó khi bạn yêu cầu.

## Đóng góp

Xem [CONTRIBUTING.md](../../CONTRIBUTING.md). Các lỗi và báo cáo hiệu năng thuộc về
[Issues](https://github.com/adriadam10/mkw-recomp/issues), các câu hỏi thuộc về
[Discussions](https://github.com/adriadam10/mkw-recomp/discussions).
