#!/usr/bin/env python3
"""Накладывает наши правки на пример Microsoft SysVAD (этап 6.2).

Запуск: python driver/patch_sysvad.py <папка Windows-driver-samples>

Скрипт работает только с текстом и ничего не скачивает. Каждая правка обязана сработать ровно один раз: если в примере
чего-то нет (сменилась версия), скрипт останавливается с понятным сообщением, а не собирает «почти то, что надо».
Повторный запуск безопасен: уже применённые правки пропускаются.

Часть 6.2a (сейчас): только имена в INF (поставщик, описание устройства, имена фильтров).
Часть 6.2b (после первой удачной сборки): число устройств 4+4 -> 6+2 (правки на C++), сбор по образцу speaker / micin.
"""
import pathlib
import sys

INX = "audio/sysvad/TabletAudioSample/ComponentizedAudioSample.inx"

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


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    return patch_inx(pathlib.Path(sys.argv[1]))


if __name__ == "__main__":
    sys.exit(main())
