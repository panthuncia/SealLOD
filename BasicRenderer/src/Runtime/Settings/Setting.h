#pragma once

#include <typeinfo>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "Runtime/Settings/ISetting.h"
#include "Runtime/Settings/SettingValueCodec.h"

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

	// Getter callable returns a void pointer to the current value, or to the copy in this thread's settings snapshot.
	std::function<void* (void)> getGetter() override {
		return [this]() -> void* {
			if (const SettingsSnapshot* snapshot = br::settings_detail::t_activeSnapshot) {
				if (void* copied = snapshot->Find(snapshotIndex)) return copied;
			}
			if (br::settings_detail::t_buildThread) br::settings_detail::ReportLiveBuildRead(snapshotIndex);
			return &value;
		};
	}

	void* liveValue() noexcept override { return &value; }

	std::unique_ptr<SettingsSnapshot::Value> copyValue() const override {
		struct Copy final : SettingsSnapshot::Value {
			explicit Copy(const T& v) : copy(v) {}
			void* Get() noexcept override { return &copy; }
			T copy;
		};
		return std::make_unique<Copy>(value);
	}

	br::extensions::SettingValue readValue() const override {
		return br::settings_detail::ToValue(value);
	}
	bool writeValue(const br::extensions::SettingValue& newValue) override {
		T converted = br::settings_detail::FromValue<T>(newValue, value);
		if (br::settings_detail::KnownEqual(converted, value)) return false;
		value = std::move(converted);
		notifyObservers();
		return true;
	}
	std::string typeName() const override {
		return br::settings_detail::TypeName<T>();
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