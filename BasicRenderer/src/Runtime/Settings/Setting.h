#pragma once

#include <typeinfo>
#include <functional>

#include "Runtime/Settings/ISetting.h"

template<typename T>
class Setting : public ISetting {
public:
	using ObserverFn = std::function<void(const T&)>;
	Setting(T initialValue) : value(initialValue) {}

	// Return type information for this setting
	const std::type_info& getType() const override {
		return typeid(T);
	}

	// Setter callable takes a void pointer to the value and casts it
	std::function<void(void*)> getSetter() override {
		return [this](void* newValuePtr) {
			value = *static_cast<T*>(newValuePtr);
			notifyObservers();
		};
	}

	// Getter callable returns a void pointer to the current value
	std::function<void* (void)> getGetter() override {
		return [this]() -> void* {
			return &value;
		};
	}

	size_t addObserver(ObserverFn obs) {
		const size_t id = ++_nextId;
		_observers.emplace_back(id, std::move(obs));
		return id;
	}
	void removeObserver(size_t id) {
		_observers.erase(
			std::remove_if(_observers.begin(), _observers.end(),
				[id](auto& e) { return e.first == id; }),
			_observers.end());
	}

private:
	T value;
	size_t _nextId = 0;
	std::vector<std::pair<size_t, ObserverFn>> _observers;

	void notifyObservers() {
		for (auto& [id, fn] : _observers)
			fn(value);
	}
};