#pragma once
namespace esp {
struct DrawMetrics {unsigned actors=0,segments=0,boxes=0;};
void Draw(bool menuVisible);
// Accessed only on the serialized rendering thread.
DrawMetrics LastDrawMetrics();
}
