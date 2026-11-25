# 构建与刷写

以下参数对应实测的微雪 **ESP32-S3-Touch-AMOLED-1.75C** 样机，Flash 为 32 MB，OPI PSRAM 为 8 MB。其他批次请先核对容量。仓库附带的正式应用镜像为 [weather_clock_175c.ino.bin](../firmware/weather_clock_175c.ino.bin)，大小 2,485,232 字节。[SHA-256 校验文件](../firmware/SHA256SUMS)

## 从源码编译

2026-11-23 实板版本使用 Arduino CLI 1.5.2-rc.1、Arduino-ESP32 3.3.10、GFX Library for Arduino 1.6.7 和 SensorLib 0.3.4。后两个库的 `src` 已逐文件核对，与 Arduino 库管理器同版压缩包一致，可以直接安装。表盘底图、指针和中文字形已随草图提供。

以下命令按 PowerShell 编写。首次克隆时使用与主草图同名的目录。已有源码时可直接进入 `weather_clock_175c` 目录，执行后续构建命令。构建产物写入系统临时目录。

```powershell
git clone https://github.com/liuyu3049-web/waveshare-175c-weather-clock.git weather_clock_175c
Set-Location weather_clock_175c
```

```powershell
$sketch = (Get-Location).Path
$build = Join-Path ([IO.Path]::GetTempPath()) 'weather-clock-175c-build'
$index = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=32M,PartitionScheme=custom,CDCOnBoot=cdc,USBMode=hwcdc,FlashMode=dio'
arduino-cli core update-index --additional-urls $index
arduino-cli core install 'esp32:esp32@3.3.10' --additional-urls $index
arduino-cli lib update-index
arduino-cli lib install 'GFX Library for Arduino@1.6.7' 'SensorLib@0.3.4'
arduino-cli compile --fqbn $fqbn --build-path $build $sketch
```

应用镜像输出为 `$build/weather_clock_175c.ino.bin`。构建使用草图中的 [partitions.csv](../partitions.csv)。发布校验和用于核对仓库附带的镜像，自行编译时工具链和构建信息可能影响二进制哈希。[乐鑫 Arduino 安装文档](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)

## 已安装相同分区布局的板子

以下步骤仅更新 `app0`，起始地址为 `0x10000`，容量为 `0xC00000`。NVS 分区中的配网和显示设置会保留。先运行 `arduino-cli board list`，将下方 `COMx` 替换为已确认的本板串口。从同一草图目录运行，刷写工具使用 esptool 5.3.1。

```powershell
py -m pip install esptool==5.3.1
$port = 'COMx'
$app = '.\firmware\weather_clock_175c.ino.bin'
$expected = ((Get-Content '.\firmware\SHA256SUMS' -Raw) -split '\s+')[0]
if ((Get-FileHash -Algorithm SHA256 $app).Hash -ne $expected) { throw '应用镜像校验失败' }

$table = [IO.Path]::GetTempFileName()
py -m esptool --chip esp32s3 --port $port read-flash 0x8000 0xC00 $table
if ($LASTEXITCODE -ne 0) { throw '无法读取板上分区表' }
if ((Get-FileHash -Algorithm SHA256 $table).Hash -ne '3D89EC6A4C1418CCE2806E1C402A66D763F4E80B95A9A29F55239349FEA400F5') { throw '板上分区表与本版不匹配' }

py -m esptool --chip esp32s3 --port $port --baud 460800 write-flash 0x10000 $app
if ($LASTEXITCODE -ne 0) { throw '写入失败' }
py -m esptool --chip esp32s3 --port $port verify-flash 0x10000 $app
```

分区校验值来自正式构建输出的 3,072 字节分区表，已与实板读取的相同范围比对。写入成功后会看到 `Hash of data verified`。[esptool 命令说明](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html)

## 首次安装到空白同型号板

空白 Flash 还需要引导程序和分区表。在完成上述源码编译并确认端口后，使用完整构建输出初始化空白板。

```powershell
$port = 'COMx'
arduino-cli upload --fqbn $fqbn --port $port --input-dir $build $sketch
```

该命令会写入引导程序（`0x0`）、分区表（`0x8000`）、OTA 初始化数据（`0xE000`）和应用（`0x10000`）。已有其他固件或数据的板子应先核对其分区和备份需求。
