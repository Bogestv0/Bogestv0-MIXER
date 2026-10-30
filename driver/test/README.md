# Проверка моста драйвера без Windows

`t.cpp` подключает настоящий `driver/src/bogestvbridge.cpp` целиком и запускает его на заглушках ядра (`sysvad.h`, `wdmsec.h` здесь
не настоящие, а макеты). Звук из «потоков» читается теми же `ShmRing`/`ShmHeader` из `engine/shm.h`, что и у движка, поэтому тест
ловит расхождение раскладки.

    cp driver/src/bogestvbridge.* driver/src/bogestv_ioctl.h driver/test/
    g++ -std=c++17 -fsanitize=address,undefined -fpermissive -w -Idriver/test -Iengine driver/test/t.cpp -o bridge_test && ./bridge_test
