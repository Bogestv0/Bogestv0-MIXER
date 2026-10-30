#!/usr/bin/env python3
"""Накладывает наши правки на пример Microsoft SysVAD (этап 6.2).

Запуск: python driver/patch_sysvad.py <папка Windows-driver-samples>

Скрипт работает только с текстом и ничего не скачивает. Каждая правка обязана сработать ровно один раз: если в примере
чего-то нет (сменилась версия), скрипт останавливается с понятным сообщением, а не собирает «почти то, что надо».
Повторный запуск безопасен: уже применённые правки пропускаются.

Часть 6.2a: имена в INF (поставщик, описание устройства, имена фильтров).
Часть 6.2b: число устройств 4+4 -> 6+2. Воспроизведение: к четырём штатным добавляются «Input 5» и «Input 6» (копии
SPDIF-пары: те же таблицы, но свои имена подустройств, свои разделы INF). Запись: остаются MicIn и MicArray1
(«Output 1», «Output 2»), массивы 2 и 3 убираются из списка устройств.
"""
import pathlib
import re
import sys

# Консоль Windows на сборочной машине в cp1252 и не печатает русский текст: принудительно UTF-8.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

INX = "audio/sysvad/TabletAudioSample/ComponentizedAudioSample.inx"
MINIPAIRS = "audio/sysvad/TabletAudioSample/minipairs.h"

# Добавляемые устройства воспроизведения (6.2b): (номер, имя в Windows)
EXTRA_INPUTS = (5, 6)

# (строка в разделе [Strings] как в примере, наша строка)
STRINGS = [
    ('ProviderName = "TODO-Set-Provider"', 'ProviderName = "Bogestv0"'),
    ('MfgName      = "TODO-Set-Manufacturer"', 'MfgName      = "Bogestv0"'),
    ('MsCopyRight  = "TODO-Set-Copyright"', 'MsCopyRight  = "Bogestv0"'),
    ('SYSVAD_SA.DeviceDesc="Virtual Audio Device (WDM) - Tablet Sample"', 'SYSVAD_SA.DeviceDesc="Bogestv0 Virtual Audio"'),
    ('SYSVAD_ComponentizedAudioSample.SvcDesc="Virtual Audio Device (WDM) - Tablet Sample Driver"',
     'SYSVAD_ComponentizedAudioSample.SvcDesc="Bogestv0 Virtual Audio Driver"'),
    # Устройства воспроизведения: их увидят программы (Spotify, игры, Discord). Пока 4 из 6 (остальные два добавит 6.2b).
    ('SYSVAD.WaveSpeaker.szPname="SYSVAD Wave Speaker"', 'SYSVAD.WaveSpeaker.szPname="Bogestv0 Input 1"'),
    ('SYSVAD.TopologySpeaker.szPname="SYSVAD Topology Speaker"', 'SYSVAD.TopologySpeaker.szPname="Bogestv0 Input 1 Topology"'),
    ('SYSVAD.WaveSpeakerHeadphone.szPname="SYSVAD Wave Speaker Headphone"', 'SYSVAD.WaveSpeakerHeadphone.szPname="Bogestv0 Input 2"'),
    ('SYSVAD.TopologySpeakerHeadphone.szPname="SYSVAD Topology Speaker Headphone"', 'SYSVAD.TopologySpeakerHeadphone.szPname="Bogestv0 Input 2 Topology"'),
    ('SYSVAD.WaveHdmi.szPname="SYSVAD Wave HDMI"', 'SYSVAD.WaveHdmi.szPname="Bogestv0 Input 3"'),
    ('SYSVAD.TopologyHdmi.szPname="SYSVAD Topology HDMI"', 'SYSVAD.TopologyHdmi.szPname="Bogestv0 Input 3 Topology"'),
    ('SYSVAD.WaveSpdif.szPname="SYSVAD Wave SPDIF"', 'SYSVAD.WaveSpdif.szPname="Bogestv0 Input 4"'),
    ('SYSVAD.TopologySpdif.szPname="SYSVAD Topology SPDIF"', 'SYSVAD.TopologySpdif.szPname="Bogestv0 Input 4 Topology"'),
    # Устройства записи: их увидят OBS и Discord как микрофоны.
    ('SYSVAD.WaveMicIn.szPname="SYSVAD Wave Microphone Headphone"', 'SYSVAD.WaveMicIn.szPname="Bogestv0 Output 1"'),
    ('SYSVAD.TopologyMicIn.szPname="SYSVAD Topology Microphone Headphone"', 'SYSVAD.TopologyMicIn.szPname="Bogestv0 Output 1 Topology"'),
    ('SYSVAD.WaveMicArray1.szPname="SYSVAD Wave Microphone Array - Front"', 'SYSVAD.WaveMicArray1.szPname="Bogestv0 Output 2"'),
    ('SYSVAD.TopologyMicArray1.szPname="SYSVAD Topology Microphone Array - Front"', 'SYSVAD.TopologyMicArray1.szPname="Bogestv0 Output 2 Topology"'),
    ('MicInCustomName= "External Microphone Headphone"', 'MicInCustomName= "Bogestv0 Output 1"'),
    ('MicArray1CustomName= "Internal Microphone Array - Front"', 'MicArray1CustomName= "Bogestv0 Output 2"'),
]


def patch_inx(root: pathlib.Path) -> int:
    path = root / INX
    if not path.exists():
        print(f"ОШИБКА: нет файла {path}. Это точно папка Windows-driver-samples нужной версии?", file=sys.stderr)
        return 1
    raw = path.read_bytes()
    enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig"
    text = raw.decode(enc)
    bad = 0
    for old, new in STRINGS:
        if text.count(old) == 1:
            text = text.replace(old, new)
        elif text.count(new) == 1:
            pass  # уже применено
        else:
            print(f"ОШИБКА: строка не найдена ровно один раз: {old}", file=sys.stderr)
            bad += 1
    if bad:
        return 1
    # Тот же формат, что был: UTF-16 с BOM (Windows так и читает .inx), переводы строк не трогаем.
    path.write_bytes(text.encode(enc))
    print(f"ok: {INX}: {len(STRINGS)} имён")
    return 0


def read_text(path: pathlib.Path):
    """Читает файл, возвращает (текст с \\n, кодировка, перевод строки), чтобы записать обратно в том же виде."""
    raw = path.read_bytes()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        enc = "utf-16"
    elif raw[:3] == b"\xef\xbb\xbf":
        enc = "utf-8-sig"
    else:
        enc = "utf-8"  # без BOM: не добавляем его при записи (иначе меняется кодировка файла для компилятора)
    text = raw.decode(enc)
    nl = "\r\n" if "\r\n" in text else "\n"
    return text.replace("\r\n", "\n"), enc, nl


def write_text(path: pathlib.Path, text: str, enc: str, nl: str) -> None:
    path.write_bytes(text.replace("\n", nl).encode(enc))


def replace_once(text: str, old: str, new: str, what: str, errors: list) -> str:
    """Меняет old на new ровно один раз. Если new уже на месте, пропускает. Иначе записывает ошибку."""
    if text.count(old) == 1:
        return text.replace(old, new)
    if text.count(new) == 1:
        return text
    errors.append(f"{what}: нужный фрагмент найден {text.count(old)} раз (ожидалось 1)")
    return text


def cpp_pair(n: int) -> str:
    return f"""static
ENDPOINT_MINIPAIR BogestvInput{n}Miniports =
{{
    eSpdifRenderDevice,                                     // device type: same as SPDIF (plain render endpoint)
    L"TopologyInput{n}",                                     // must match KSNAME_TopologyInput{n} in the inf [Strings] section
    NULL,                                                   // optional template name
    CreateMiniportTopologySYSVAD,
    &SpdifTopoMiniportFilterDescriptor,
    0, NULL,                                                // Interface properties
    L"WaveInput{n}",                                         // must match KSNAME_WaveInput{n} in the inf [Strings] section
    NULL,                                                   // optional template name
    CreateMiniportWaveRTSYSVAD,
    &SpdifWaveMiniportFilterDescriptor,
    ARRAYSIZE(SysvadWaveFilterInterfacePropertiesRender),   // Interface properties
    SysvadWaveFilterInterfacePropertiesRender,
    SPDIF_DEVICE_MAX_CHANNELS,
    SpdifPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpdifPinDeviceFormatsAndModes),
    SpdifTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpdifTopologyPhysicalConnections),
    ENDPOINT_OFFLOAD_SUPPORTED,
    NULL, 0, NULL,                                          // audio module settings.
}};

"""


def patch_minipairs(root: pathlib.Path) -> int:
    path = root / MINIPAIRS
    if not path.exists():
        print(f"ОШИБКА: нет файла {path}", file=sys.stderr)
        return 1
    text, enc, nl = read_text(path)
    errors: list = []

    # 1. Две новые пары воспроизведения: вставляем перед разделом «Capture miniports».
    block = ("//\n// Bogestv0: extra render endpoints (Input 5 and Input 6), copies of the SPDIF pair.\n//\n"
             + "".join(cpp_pair(n) for n in EXTRA_INPUTS))
    anchor = "//\n// Capture miniports.\n//\n"
    if "BogestvInput5Miniports =" in text:
        pass
    elif text.count(anchor) == 1:
        text = text.replace(anchor, block + anchor)
    else:
        errors.append("minipairs.h: не найден раздел «Capture miniports»")

    # 2. Список устройств воспроизведения: 4 -> 6.
    text = replace_once(
        text,
        "    &SpdifMiniports,\n};",
        "    &SpdifMiniports,\n    &BogestvInput5Miniports,\n    &BogestvInput6Miniports,\n};",
        "список g_RenderEndpoints", errors)

    # 3. Список устройств записи: 4 -> 2 (остаются MicIn и MicArray1).
    text = replace_once(
        text,
        "    &MicArray1Miniports,\n    &MicArray2Miniports,\n    &MicArray3Miniports,\n};",
        "    &MicArray1Miniports,\n};",
        "список g_CaptureEndpoints", errors)

    if errors:
        for e in errors:
            print("ОШИБКА: " + e, file=sys.stderr)
        return 1
    write_text(path, text, enc, nl)
    print(f"ok: {MINIPAIRS}: воспроизведение 6, запись 2")
    return 0


def inx_extra_sections() -> str:
    out = []
    for n in EXTRA_INPUTS:
        out.append(f""";======================================================
; render interfaces: Bogestv0 Input {n}
;======================================================
[SYSVAD.I.WaveInput{n}]
AddReg=SYSVAD.I.WaveInput{n}.AddReg
[SYSVAD.I.WaveInput{n}.AddReg]
HKR,,CLSID,,%Proxy.CLSID%
HKR,,FriendlyName,,%SYSVAD.WaveInput{n}.szPname%

[SYSVAD.I.TopologyInput{n}]
AddReg=SYSVAD.I.TopologyInput{n}.AddReg
[SYSVAD.I.TopologyInput{n}.AddReg]
HKR,,CLSID,,%Proxy.CLSID%
HKR,,FriendlyName,,%SYSVAD.TopologyInput{n}.szPname%
; The following lines opt-in to pull mode.
HKR,EP\\0,%PKEY_AudioEndpoint_Association%,,%KSNODETYPE_ANY%
HKR,EP\\0,%PKEY_AudioEndpoint_Supports_EventDriven_Mode%,0x00010001,0x1

""")
    return "".join(out)


def inx_extra_interfaces() -> str:
    out = []
    for n in EXTRA_INPUTS:
        out.append(f""";
; Interfaces for Bogestv0 Input {n} render endpoint.
;
AddInterface=%KSCATEGORY_AUDIO%, %KSNAME_WaveInput{n}%, SYSVAD.I.WaveInput{n}
AddInterface=%KSCATEGORY_RENDER%, %KSNAME_WaveInput{n}%, SYSVAD.I.WaveInput{n}
AddInterface=%KSCATEGORY_REALTIME%, %KSNAME_WaveInput{n}%, SYSVAD.I.WaveInput{n}
AddInterface=%KSCATEGORY_AUDIO%, %KSNAME_TopologyInput{n}%, SYSVAD.I.TopologyInput{n}
AddInterface=%KSCATEGORY_TOPOLOGY%, %KSNAME_TopologyInput{n}%, SYSVAD.I.TopologyInput{n}

""")
    return "".join(out)


def patch_inx_devices(root: pathlib.Path) -> int:
    """6.2b: разделы INF для Input 5 и 6. Запускать после patch_inx (опирается на уже переименованные строки)."""
    path = root / INX
    text, enc, nl = read_text(path)
    errors: list = []

    # Разделы с интерфейсами: перед блоком «speaker (external: headphone)».
    a = (";======================================================\n; render interfaces: speaker (external: headphone)\n")
    if "[SYSVAD.I.WaveInput5]" not in text:
        if text.count(a) == 1:
            text = text.replace(a, inx_extra_sections() + a)
        else:
            errors.append("INF: не найден раздел «render interfaces: speaker (external: headphone)»")

    # Строки AddInterface: сразу после интерфейсов SPDIF (его последняя строка уникальна).
    last = "AddInterface=%KSCATEGORY_TOPOLOGY%, %KSNAME_TopologySpdif%, SYSVAD.I.TopologySpdif\n"
    if "%KSNAME_WaveInput5%, SYSVAD.I.WaveInput5" not in text:
        if text.count(last) == 1:
            text = text.replace(last, last + "\n" + inx_extra_interfaces().rstrip("\n") + "\n")
        else:
            errors.append("INF: не найдены AddInterface для SPDIF")

    # Имена подустройств (раздел [Strings], «нелокализуемое»).
    k = 'KSNAME_TopologySpdif= "TopologySpdif"\n'
    if 'KSNAME_WaveInput5=' not in text:
        if text.count(k) == 1:
            add = "".join(f'\nKSNAME_WaveInput{n}="WaveInput{n}"\nKSNAME_TopologyInput{n}="TopologyInput{n}"\n' for n in EXTRA_INPUTS)
            text = text.replace(k, k + add)
        else:
            errors.append("INF: не найдена строка KSNAME_TopologySpdif")

    # Видимые имена (после 6.2a строка SPDIF уже называется «Bogestv0 Input 4 Topology»).
    t = 'SYSVAD.TopologySpdif.szPname="Bogestv0 Input 4 Topology"\n'
    if 'SYSVAD.WaveInput5.szPname="' not in text:
        if text.count(t) == 1:
            add = "".join(f'SYSVAD.WaveInput{n}.szPname="Bogestv0 Input {n}"\nSYSVAD.TopologyInput{n}.szPname="Bogestv0 Input {n} Topology"\n'
                          for n in EXTRA_INPUTS)
            text = text.replace(t, t + add)
        else:
            errors.append("INF: нет строки SPDIF после переименования (6.2a должна пройти раньше)")

    # Самопроверка: каждый %токен% вида SYSVAD.*.szPname / KSNAME_* обязан быть определён в [Strings].
    for tok in sorted(set(re.findall(r"%((?:SYSVAD\.\w+\.szPname)|(?:KSNAME_\w+))%", text))):
        if not re.search(r"^" + re.escape(tok) + r"\s*=", text, re.M):
            errors.append(f"INF: строка {tok} не определена в [Strings]")

    if errors:
        for e in errors:
            print("ОШИБКА: " + e, file=sys.stderr)
        return 1
    write_text(path, text, enc, nl)
    print(f"ok: {INX}: устройства Input {EXTRA_INPUTS[0]}–{EXTRA_INPUTS[-1]}")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = pathlib.Path(sys.argv[1])
    for step in (patch_inx, patch_inx_devices, patch_minipairs):
        rc = step(root)
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
