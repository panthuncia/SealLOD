#pragma once

#include <typeinfo>

class ISetting {
public:
    virtual ~ISetting() = default;

    // Virtual method to retrieve the type of the setting
    virtual const std::type_info& getType() const = 0;

    // Function to retrieve a callable for setting the value
    virtual std::function<void(void*)> getSetter() = 0;

    // Function to retrieve a callable for getting the value
    virtual std::function<void* (void)> getGetter() = 0;
};