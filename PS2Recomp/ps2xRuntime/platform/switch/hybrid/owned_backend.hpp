#pragma once
#include <functional>
namespace native_renderer::detail {
class Backend;
// CPU frontend records immutable uploads/draws; one real native worker creates,
// submits and destroys GPU resources. The factory is invoked on that worker.
Backend *createOwnedDekoBackend(std::function<Backend *()> factory);
}
