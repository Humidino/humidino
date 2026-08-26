#pragma once

// Строит и обновляет единственный экран дашборда: строка статуса (время
// работы, WiFi, ОЗУ), сетка 2x2 из панелей зон и цветной баннер статуса,
// зависящий от RelayControlState. Вызывается из display.cpp (lvglTask).
namespace UiDashboard {

void build();
void update();  // читает актуальный SystemState и обновляет все подписи

}  // namespace UiDashboard
