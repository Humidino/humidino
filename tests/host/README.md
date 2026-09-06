# Проверки интерфейса и реле без ESP32

Используются настоящий LVGL из зависимостей PlatformIO, виджеты главного
экрана, обработчики режимов, SettingsActions и RelayController. GPIO, часы,
общее состояние, сохранение и журнал заменены заглушками. Проверка касаний
проходит через pointer input LVGL, а не прямой вызов обработчиков кнопок.
Физический тачскрин, FreeRTOS, NVS и электрическая часть здесь не проверяются.

Отдельный тест подсветки запускает настоящий `backlight.cpp` с подменой
часов и PWM: гашение, мгновенное пробуждение, удержание касания, прерывание
гашения и переполнение счётчика миллисекунд.

После установки зависимостей прошивки (`pio run -e esp32-s3-devkitc-1`),
из корня проекта с CMake и компилятором C/C++:

```powershell
cmake -S tests/host -B .pio/host-checks
cmake --build .pio/host-checks --config Debug --parallel
ctest --test-dir .pio/host-checks -C Debug --output-on-failure
```
