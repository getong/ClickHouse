#pragma once

#include <Core/Block_fwd.h>
#include <Processors/IProcessor.h>


namespace DB
{

/** Has arbitrary non zero number of inputs and one output.
  * All of them have the same structure.
  *
  * Pulls all data from first input, then all data from second input, etc...
  * Doesn't do any heavy calculations.
  * Preserves an order of data.
  */
class ConcatProcessor final : public IProcessor
{
public:
    ConcatProcessor(SharedHeader header, size_t num_inputs);

    String getName() const override { return "Concat"; }

    Status prepare() override;

    OutputPort & getOutputPort() { return outputs.front(); }

private:
    InputPorts::iterator current_input;
};

}

