#pragma once
namespace rex::ui {
class WindowedAppContext;
class Presenter;
namespace metal {
// Disabled unless FABLE2_TEST_CONTROL_DIR is set. App-owned test API only.
void StartMetalTestControl(WindowedAppContext* context, Presenter* presenter);
}
}
