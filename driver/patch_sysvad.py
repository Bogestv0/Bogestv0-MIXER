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
Часть 6.4: мост с общей памятью движка. В EndpointsCommon добавляются наши файлы (driver/src), потоки SysVAD вместо тона и записи
в файл гоняют звук через кольца общей памяти, драйвер создаёт управляющее устройство Bogestv0Mixer, через которое движок получает память.
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

# Имена конечных точек воспроизведения (6.2c): номер -> GUID имени
INPUT_NAME_GUIDS = {
    1: "ee448946-08cb-42d4-bc7f-3af2a4631318",
    2: "ae92c08c-0659-41b8-a88b-4dcc71758bdd",
    3: "03ec7245-2551-4c1e-8152-333787bcdb08",
    4: "7181f591-1538-4bfc-92ed-1967c1947b64",
    5: "0c661e94-8108-472b-8d2f-2858eb28a51f",
    6: "12ec3d4d-feb2-4fa3-be5d-c64344755b26",
}
TOPTABLES = {  # номер -> (файл таблицы топологии, категория контакта-выхода)
    1: ("audio/sysvad/EndpointsCommon/speakertoptable.h", "KSNODETYPE_SPEAKER"),
    2: ("audio/sysvad/EndpointsCommon/speakerhptoptable.h", "KSNODETYPE_HEADPHONES"),
    3: ("audio/sysvad/TabletAudioSample/hdmitoptable.h", "KSNODETYPE_HDMI_INTERFACE"),
    4: ("audio/sysvad/TabletAudioSample/spdiftoptable.h", "KSNODETYPE_SPDIF_INTERFACE"),
}
SPDIF_TOPTABLE = "audio/sysvad/TabletAudioSample/spdiftoptable.h"

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
    &BogestvIn{n}TopoMiniportFilterDescriptor,
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

    # 1b. Копии таблицы топологии SPDIF для Input 5 и 6 (6.2c).
    inc_anchor = '#include "spdifwavtable.h"\n'
    inc_new = "".join(f'#include "bogestvin{n}toptable.h"\n' for n in EXTRA_INPUTS)
    if inc_new in text:
        pass
    elif text.count(inc_anchor) == 1:
        text = text.replace(inc_anchor, inc_anchor + inc_new)
    else:
        errors.append("minipairs.h: не найден #include spdifwavtable.h")

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



def guid_define(n: int) -> str:
    g = INPUT_NAME_GUIDS[n]
    a = g.split("-")
    tail = a[3] + a[4]
    bytes_ = ", ".join("0x" + tail[i:i + 2] for i in range(0, 16, 2))
    return (f"\n//\n// {{{g}}}  (Bogestv0 Input {n}: endpoint name)\n"
            f"DEFINE_GUID(BOGESTV_INPUT{n}_NAME,\n0x{a[0]}, 0x{a[1]}, 0x{a[2]}, {bytes_});\n")


def add_guid_after_guard(text: str, n: int, errors: list, what: str) -> str:
    marker = f"BOGESTV_INPUT{n}_NAME"
    if marker in text:
        return text
    m = re.search(r"#define _SYSVAD_\w+_H_\n", text)
    if not m:
        errors.append(f"{what}: не найден #define защиты от повторного включения")
        return text
    return text[:m.end()] + guid_define(n) + text[m.end():]


def set_pin_name(text: str, category: str, n: int, errors: list, what: str) -> str:
    """Контакт-выход с категорией <category>: Name NULL -> &BOGESTV_INPUT<n>_NAME."""
    pat = re.compile(r"(&" + re.escape(category) + r",\s*// Category\n\s*)NULL(,?\s*// Name)")
    if f"&BOGESTV_INPUT{n}_NAME" in text:
        return text
    if len(pat.findall(text)) != 1:
        errors.append(f"{what}: контакт с категорией {category} найден {len(pat.findall(text))} раз (ожидалось 1)")
        return text
    return pat.sub(lambda m: m.group(1) + f"&BOGESTV_INPUT{n}_NAME," + m.group(2).lstrip(","), text)


def patch_names(root: pathlib.Path) -> int:
    """6.2c: имена Input 1..6 для устройств воспроизведения."""
    errors: list = []

    # Копии таблицы SPDIF для Input 5 и 6: делаем из нетронутой таблицы (до вставки имени).
    sp_path = root / SPDIF_TOPTABLE
    if not sp_path.exists():
        print(f"ОШИБКА: нет файла {sp_path}", file=sys.stderr)
        return 1
    sp_text, sp_enc, sp_nl = read_text(sp_path)
    for n in EXTRA_INPUTS:
        dst = root / f"audio/sysvad/TabletAudioSample/bogestvin{n}toptable.h"
        if dst.exists():
            continue
        if f"BOGESTV_INPUT4_NAME" in sp_text:
            errors.append("таблица SPDIF уже изменена, а копии для Input 5/6 не созданы: начните с чистого примера")
            continue

        def ren(m):
            tok = m.group(0)
            if tok.startswith("PropertyHandler_"):
                return tok  # обработчик свойств общий с SPDIF
            return tok.replace("Spdif", f"BogestvIn{n}")
        t = re.sub(r"\b\w*Spdif\w*\b", ren, sp_text)
        t = t.replace("SPDIFTOPTABLE", f"BOGESTVIN{n}TOPTABLE")
        t = add_guid_after_guard(t, n, errors, f"bogestvin{n}toptable.h")
        t = set_pin_name(t, "KSNODETYPE_SPDIF_INTERFACE", n, errors, f"bogestvin{n}toptable.h")
        write_text(dst, t, sp_enc, sp_nl)
        print(f"ok: создан bogestvin{n}toptable.h")

    # Имена в таблицах штатных устройств.
    for n, (rel, cat) in TOPTABLES.items():
        path = root / rel
        if not path.exists():
            errors.append(f"нет файла {rel}")
            continue
        t, enc, nl = read_text(path)
        t = add_guid_after_guard(t, n, errors, rel)
        t = set_pin_name(t, cat, n, errors, rel)
        write_text(path, t, enc, nl)

    # Input 1 (динамики): Windows не применяет своё имя к «встроенному» устройству (ePortConnIntegratedDevice),
    # поэтому делаем его внешним разъёмом, как наушники. Input 3 (HDMI): имя берётся из описания монитора.
    for rel, old, new, what in (
        ("audio/sysvad/EndpointsCommon/speakertoptable.h",
         "    eGeoLocFront,\n    eGenLocPrimaryBox,\n    ePortConnIntegratedDevice,",
         "    eGeoLocFront,\n    eGenLocPrimaryBox,\n    ePortConnJack,",
         "тип разъёма динамиков"),
        ("audio/sysvad/TabletAudioSample/hdmitopo.cpp",
         'L"SinkDescription Sample"', 'L"Bogestv0 Input 3"', "описание монитора HDMI"),
    ):
        fp = root / rel
        if not fp.exists():
            errors.append(f"нет файла {rel}")
            continue
        ft, fenc, fnl = read_text(fp)
        ft = replace_once(ft, old, new, what, errors)
        write_text(fp, ft, fenc, fnl)

    # INF: регистрация имён.
    path = root / INX
    t, enc, nl = read_text(path)
    reg_anchor = "HKR,%MEDIA_CATEGORIES%\\%MicInCustomNameGUID%,Name,,%MicInCustomName%\n"
    if "%Input1CustomNameGUID%" not in t:
        if t.count(reg_anchor) == 1:
            add = "".join(f"HKR,%MEDIA_CATEGORIES%\\%Input{n}CustomNameGUID%,Name,,%Input{n}CustomName%\n" for n in range(1, 7))
            t = t.replace(reg_anchor, reg_anchor + add)
        else:
            errors.append("INF: не найдена строка регистрации имени MicIn")
    guid_anchor = re.search(r"^MicArray3CustomNameGUID\s*=.*\n", t, re.M)
    if "Input1CustomNameGUID =" not in t:
        if guid_anchor:
            add = "".join(f"Input{n}CustomNameGUID = {{{INPUT_NAME_GUIDS[n]}}}\n" for n in range(1, 7))
            t = t[:guid_anchor.end()] + add + t[guid_anchor.end():]
        else:
            errors.append("INF: не найдена строка MicArray3CustomNameGUID")
    name_anchor = re.search(r'^MicInCustomName\s*=.*\n', t, re.M)
    if 'Input1CustomName=' not in t:
        if name_anchor:
            add = "".join(f'Input{n}CustomName= "Bogestv0 Input {n}"\n' for n in range(1, 7))
            t = t[:name_anchor.end()] + add + t[name_anchor.end():]
        else:
            errors.append("INF: не найдена строка MicInCustomName")
    for tok in sorted(set(re.findall(r"%(Input\d(?:CustomName|CustomNameGUID))%", t))):
        if not re.search(r"^" + tok + r"\s*=", t, re.M):
            errors.append(f"INF: {tok} не определена в [Strings]")

    if errors:
        for e in errors:
            print("ОШИБКА: " + e, file=sys.stderr)
        return 1
    write_text(path, t, enc, nl)
    print("ok: имена Input 1–6 (таблицы топологии, разъём динамиков, описание HDMI, INF)")
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# 6.4: мост с общей памятью движка
# ---------------------------------------------------------------------------------------------------------------------
BRIDGE_SRC = pathlib.Path(__file__).resolve().parent / "src"
BRIDGE_FILES = ("bogestvbridge.cpp", "bogestvbridge.h", "bogestv_ioctl.h")
SYSVAD = "audio/sysvad"


def insert_once(text: str, anchor: str, add: str, what: str, errors: list, before: bool = False) -> str:
    """Вставляет add рядом с anchor (после него или до). Если add уже на месте, пропускает. anchor обязан встретиться ровно один раз."""
    if add in text:
        return text
    if text.count(anchor) != 1:
        errors.append(f"{what}: опорный фрагмент найден {text.count(anchor)} раз (ожидалось 1)")
        return text
    return text.replace(anchor, add + anchor if before else anchor + add)


def patch_bridge(root: pathlib.Path) -> int:
    errors: list = []
    base = root / SYSVAD
    # 1. наши файлы -> EndpointsCommon (ту же папку видит и TabletAudioSample через пути включения)
    for name in BRIDGE_FILES:
        src = BRIDGE_SRC / name
        if not src.exists():
            errors.append(f"нет нашего файла {src}")
            continue
        raw = src.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")   # как весь SysVAD: CRLF
        (base / "EndpointsCommon" / name).write_bytes(raw)

    # 2. проекты
    path = base / "EndpointsCommon/EndpointsCommon.vcxproj"
    t, enc, nl = read_text(path)
    t = insert_once(t, '    <ClCompile Include="a2dphpminwavert.cpp" />\n', '    <ClCompile Include="bogestvbridge.cpp" />\n', "EndpointsCommon.vcxproj", errors)
    write_text(path, t, enc, nl)

    path = base / "TabletAudioSample/TabletAudioSample.vcxproj"
    t, enc, nl = read_text(path)
    old = "$(DDK_LIB_PATH)\\portcls.lib"
    if old not in t:
        errors.append("TabletAudioSample.vcxproj: нет portcls.lib в зависимостях")
    elif "wdmsec.lib" not in t:
        t = t.replace(old, "$(DDK_LIB_PATH)\\wdmsec.lib;" + old)
    write_text(path, t, enc, nl)

    # 3. adapter.cpp: запуск и выгрузка моста
    path = base / "adapter.cpp"
    t, enc, nl = read_text(path)
    t = insert_once(t, '#include "simple.h"\n', '#include "bogestvbridge.h"\n', "adapter.cpp: include", errors)
    t = insert_once(t, "    DriverObject->DriverUnload = DriverUnload;\n",
                    "\n    //\n    // Bogestv0: общая память и устройство для движка (этап 6.4). Ошибка не фатальна: без моста SysVAD работает как раньше.\n    //\n    BogestvBridgeInit(DriverObject);\n",
                    "adapter.cpp: BogestvBridgeInit", errors)
    t = insert_once(t, '    DPF(D_TERSE, ("[DriverUnload]"));\n', "\n    BogestvBridgeUnload();\n", "adapter.cpp: BogestvBridgeUnload", errors)
    write_text(path, t, enc, nl)

    # 4. поток: заголовок
    path = base / "EndpointsCommon/minwavertstream.h"
    t, enc, nl = read_text(path)
    t = insert_once(t, '#include "tonegenerator.h"\n', '#include "bogestvbridge.h"\n', "minwavertstream.h: include", errors)
    t = insert_once(t, "    ToneGenerator               m_ToneGenerator;\n",
                    "    LONG                        m_bgRing1;      // Bogestv0: номер кольца + 1 (0 = поток не наш)\n"
                    "    BOOLEAN                     m_bgSync;\n"
                    "    ULONGLONG                   m_bgFrames;     // сколько кадров уже передано в кольцо / взято из кольца\n"
                    "    BG_FMT                      m_bgFmt;\n",
                    "minwavertstream.h: поля", errors)
    t = insert_once(t, "    VOID WriteBytes\n", "    VOID BgPump(_In_ ULONG ByteDisplacement);\n", "minwavertstream.h: BgPump", errors, before=True)
    write_text(path, t, enc, nl)

    # 5. поток: код
    path = base / "EndpointsCommon/minwavertstream.cpp"
    t, enc, nl = read_text(path)
    t = insert_once(t, "    RtlCopyMemory(m_pWfExt, pWfEx, sizeof(WAVEFORMATEX) + pWfEx->cbSize);\n",
                    "\n    // Bogestv0: наш ли это поток (по имени подустройства), и в какой он формат.\n"
                    "    m_bgRing1 = 0;\n    m_bgSync = TRUE;\n    m_bgFrames = 0;\n"
                    "    if (!m_pMiniport->IsOffloadPin(Pin_) && !m_pMiniport->IsKeywordDetectorPin(Pin_) && !(Capture_ && m_pMiniport->IsLoopbackPin(Pin_)))\n"
                    "    {\n"
                    "        LONG bgRing = BogestvRingFor(m_pMiniport->m_pMiniportPair->WaveName, Capture_);\n"
                    "        if (bgRing >= 0)\n"
                    "        {\n"
                    "            BogestvFillFmt(&m_bgFmt, m_pWfExt);\n"
                    "            m_bgRing1 = bgRing + 1;\n"
                    "        }\n"
                    "    }\n",
                    "minwavertstream.cpp: Init", errors)
    t = insert_once(t, "    m_KsState = State_;\n\n#if defined(SYSVAD_BTH_BYPASS)",
                    "    if (m_bgRing1 > 0)\n    {\n        m_bgSync = TRUE;\n        BogestvStreamRun(m_bgRing1 - 1, m_bCapture, (State_ == KSSTATE_RUN), &m_bgFmt);\n    }\n\n",
                    "minwavertstream.cpp: SetState", errors, before=True)
    # деструктор: поток закрыт, кольцо больше не активно
    t = insert_once(t, "    if (NULL != m_pMiniport)\n    {\n        if (m_pAudioModules)\n",
                    "    if (m_bgRing1 > 0)\n    {\n        BogestvStreamRun(m_bgRing1 - 1, m_bCapture, FALSE, NULL);\n        m_bgRing1 = 0;\n    }\n",
                    "minwavertstream.cpp: деструктор", errors, before=True)
    # воспроизведение: читаем буфер всегда (а не только при записи файлов)
    t = replace_once(t, "        if (!g_DoNotCreateDataFiles)\n        {\n            // Read from buffer and write to a file.",
                     "        if (!g_DoNotCreateDataFiles || m_bgRing1 > 0)\n        {\n            // Read from buffer and write to a file.",
                     "minwavertstream.cpp: условие чтения", errors)
    write_text(path, t, enc, nl)
    return finish_bridge(path, errors)


BGPUMP = """
//=============================================================================
#pragma code_seg()
VOID CMiniportWaveRTStream::BgPump
(
    _In_ ULONG ByteDisplacement
)
/*++

Bogestv0 (этап 6.4): переносит кадры между буфером DMA потока и кольцом общей памяти.
Воспроизведение: кадры, которые «проиграло» бы устройство, уходят в кольцо (их заберёт движок).
Запись: буфер заполняется звуком из кольца (его туда кладёт движок).

--*/
{
    const ULONG blockAlign = m_pWfExt ? m_pWfExt->Format.nBlockAlign : 0;
    if (blockAlign == 0 || m_ulDmaBufferSize == 0 || m_pDmaBuffer == NULL || m_bgRing1 <= 0)
    {
        return;
    }

    const ULONGLONG curFrame = m_ullLinearPosition / blockAlign;
    const ULONGLONG endFrame = (m_ullLinearPosition + ByteDisplacement) / blockAlign;
    const ULONGLONG bufFrames = m_ulDmaBufferSize / blockAlign;

    // Первый вызов после запуска или сброс позиции: начинаем с текущего кадра.
    if (m_bgSync || m_bgFrames > endFrame)
    {
        m_bgFrames = curFrame;
        m_bgSync = FALSE;
    }
    // Отстали больше, чем размер буфера: старое уже перезаписано.
    if (endFrame - m_bgFrames > bufFrames)
    {
        m_bgFrames = endFrame - bufFrames;
    }

    const ULONG frames = (ULONG)(endFrame - m_bgFrames);
    if (frames > 0)
    {
        if (m_bCapture)
        {
            BogestvCapture(m_bgRing1 - 1, m_pDmaBuffer, m_ulDmaBufferSize, m_bgFrames, frames, &m_bgFmt);
        }
        else
        {
            BogestvRender(m_bgRing1 - 1, m_pDmaBuffer, m_ulDmaBufferSize, m_bgFrames, frames, &m_bgFmt);
        }
        m_bgFrames = endFrame;
    }
}
"""


def finish_bridge(path: pathlib.Path, errors: list) -> int:
    """Достраивает minwavertstream.cpp: хуки в WriteBytes/ReadBytes и тело BgPump."""
    t, enc, nl = read_text(path)
    hook = "    if (m_bgRing1 > 0)\n    {\n        BgPump(ByteDisplacement);\n        return;\n    }\n\n"
    anchors = [
        ("    ULONG bufferOffset = m_ullLinearPosition % m_ulDmaBufferSize;\n\n    // Normally this will loop no more than once for a single wrap, but if\n"
         "    // many bytes have been displaced then this may loops many times.\n    while (ByteDisplacement > 0)\n    {\n"
         "        ULONG runWrite = min(ByteDisplacement, m_ulDmaBufferSize - bufferOffset);\n            m_ToneGenerator", "WriteBytes"),
        ("    ULONG bufferOffset = m_ullLinearPosition % m_ulDmaBufferSize;\n\n    // Normally this will loop no more than once for a single wrap, but if\n"
         "    // many bytes have been displaced then this may loops many times.\n    while (ByteDisplacement > 0)\n    {\n"
         "        ULONG runWrite = min(ByteDisplacement, m_ulDmaBufferSize - bufferOffset);\n        m_SaveData", "ReadBytes"),
    ]
    for anchor, what in anchors:
        marker = hook + anchor
        if marker in t:
            continue
        if t.count(anchor) != 1:
            errors.append(f"minwavertstream.cpp: {what}: опорный фрагмент найден {t.count(anchor)} раз (ожидалось 1)")
            continue
        t = t.replace(anchor, hook + anchor)
    if "CMiniportWaveRTStream::BgPump" not in t:
        t = t.rstrip("\n") + "\n" + BGPUMP
    if errors:
        for e in errors:
            print("ОШИБКА: " + e, file=sys.stderr)
        return 1
    write_text(path, t, enc, nl)
    print("ok: мост с общей памятью (6.4): файлы, проекты, adapter.cpp, потоки")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = pathlib.Path(sys.argv[1])
    for step in (patch_inx, patch_inx_devices, patch_minipairs, patch_names, patch_bridge):
        rc = step(root)
        if rc:
            return rc
    return 0


if __name__ == "__main__":
    sys.exit(main())
