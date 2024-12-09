#include "dancer.hpp"

using namespace sysio;

void dancer::dance() {
    print("Dancer is dancing");
    steps stepsTable(_self, _self.value);
    auto lastStepMaybe = stepsTable.find(_self.value);
    if(lastStepMaybe == stepsTable.end()) {
        stepsTable.emplace(_self, [&]( auto& obj ) {
            obj.id = _self.value;
            obj.count = 1;
            obj.is_dancing = true;
        });
    } else {
        const auto& lastStep = *lastStepMaybe;
        stepsTable.modify(lastStep, _self, [&]( auto& obj ) {
            obj.count = lastStep.count + 1;
            obj.is_dancing = true;
        });
    }
}

void dancer::stop() {
    print("Dancer is stopping");
    steps stepsTable(_self, _self.value);
    const auto& lastStep = stepsTable.get(_self.value, "no dance to stop!");
    stepsTable.modify(lastStep, _self, [&]( auto& obj ) {
        obj.count = 0;
        obj.is_dancing = false;
    });
}
