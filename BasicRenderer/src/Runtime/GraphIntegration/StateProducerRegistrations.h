#pragma once

namespace br::render {
class AsyncStateGraph;
void RegisterLightStateProducer(AsyncStateGraph& graph);
void RegisterViewStateProducer(AsyncStateGraph& graph);
}
