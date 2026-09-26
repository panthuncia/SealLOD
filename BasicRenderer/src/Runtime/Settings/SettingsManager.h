#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <typeinfo>
#include <type_traits>
#include <stdexcept>
#include <atomic>
#include <cstdint>

#include <BasicRenderer/Extensions/SettingAccess.h>

#include "Runtime/Settings/Setting.h"

class SettingsManager {
public:
    static SettingsManager& GetInstance();
    uint64_t Revision() const noexcept { return m_revision.load(std::memory_order_acquire); }

    // Registers a setting with the given name and initial value
    template<typename T>
    void registerSetting(const std::string& name, T initialValue) {
        settings[name] = std::make_unique<Setting<T>>(initialValue);
    }

    // Returns a setter callable for the specified setting by name
    template<typename T>
    std::function<void(T)> getSettingSetter(const std::string& name) {
        auto& setting = getSettingByName(name);

        // Type check: Ensure that the requested type matches the stored type
        if (setting.getType() != typeid(T)) {
            throw std::runtime_error("Type mismatch for setting: " + name);
        }

        auto setter = setting.getSetter();
        return [this, setter](T newValue) {
            setter(static_cast<void*>(&newValue));
            m_revision.fetch_add(1, std::memory_order_release);
        };
    }

    // Returns a getter callable for the specified setting by name
    template<typename T>
    std::function<T(void)> getSettingGetter(const std::string& name) {
        auto& setting = getSettingByName(name);


        // Cast and return the getter
        auto getter = setting.getGetter();
        return [getter]() -> T {
            return static_cast<T>(*static_cast<typename std::remove_reference<T>::type*>(getter()));
        };
    }

    using Subscription = br::extensions::SettingSubscription;

    template<typename T>
    Subscription addObserver(const std::string& name,
        std::function<void(const T&)> obs)
    {
        auto& s = dynamic_cast<Setting<T>&>(getSettingByName(name));
        size_t id = s.addObserver(std::move(obs));
        // capture everything we need to remove it later
        return Subscription([this, name, id]() {
            removeObserver<T>(name, id);
            });
    }

    template<typename T>
    void removeObserver(const std::string& name, size_t id) {
        auto& s = dynamic_cast<Setting<T>&>(getSettingByName(name));
        s.removeObserver(id);
    }

    // Registers a dependency where 'controlledName' is updated based on 'controllerName' changing.
    // The resolver function takes (newControllerValue, currentControlledValue) and returns the newControlledValue.
    template<typename TController, typename TControlled>
    void registerDependency(const std::string& controllerName, const std::string& controlledName,
        std::function<TControlled(const TController&, const TControlled&)> resolver)
    {
        auto sub = addObserver<TController>(controllerName,
            [this, controlledName, resolver](const TController& controllerVal) {
                auto getter = getSettingGetter<TControlled>(controlledName);
                TControlled currentVal = getter();
                TControlled newVal = resolver(controllerVal, currentVal);
                if (newVal != currentVal) {
                    getSettingSetter<TControlled>(controlledName)(newVal);
                }
            });
        m_dependencySubscriptions.push_back(std::move(sub));
    }

    // Convenience helpers for common logical constraint patterns.
    //
    // Naming notes:
    // - "Implication" means A ⇒ B (if A then B).
    // - "Equivalence" means A ⇔ B (A if-and-only-if B).
    // - "Exclusion" means ¬(A ∧ B) (not both true).
    // - These helpers *enforce* constraints by writing settings when violated.
    //
    // Important: if you want a constraint to hold no matter which side the user edits,
    // you typically need dependencies in both directions (or a dedicated observer-based solver).

    // -----------------------------------------------------------------------------
    // Generic value implication: (controller == requiredValue) ⇒ (controlled = impliedValue)
    // If antecedent is false, controlled is left unchanged.
    template<typename TController, typename TControlled>
    void addImplicationEqValue(const std::string& controllerName,
        const std::string& controlledName,
        const TController& requiredValue,
        const TControlled& impliedValue)
    {
        registerDependency<TController, TControlled>(
            controllerName,
            controlledName,
            [requiredValue, impliedValue](const TController& ctrl, const TControlled& current) {
                // (ctrl == requiredValue) ⇒ (controlled := impliedValue)
                return (ctrl == requiredValue) ? impliedValue : current;
            });
    }

    // Generic "functional dependence":
    // controlled := f(controller, currentControlled)
    template<typename TController, typename TControlled, typename F>
    void addFunctionalDependency(const std::string& controllerName,
        const std::string& controlledName,
        F&& computeNewValue)
    {
        registerDependency<TController, TControlled>(
            controllerName,
            controlledName,
            [fn = std::forward<F>(computeNewValue)](const TController& ctrl, const TControlled& current) {
                return fn(ctrl, current);
            });
    }

    // -----------------------------------------------------------------------------
    // Bool implication: A ⇒ B
    //
    // - If A becomes true, force B true.
    // - If B becomes false, force A false.
    //
    //  A on  => B on
    //  B off => A off
    //  A off => B unconstrained
    void addImplicationConstraint(const std::string& antecedentName,
        const std::string& consequentName)
    {
        // Antecedent → Consequent : A ⇒ B
        registerDependency<bool, bool>(
            antecedentName,
            consequentName,
            [](bool A, bool Bcur) {
                // A ⇒ B  (if A is true, force B true)
                return A ? true : Bcur;
            });

        // Contrapositive (enforcement direction): ¬B ⇒ ¬A
        registerDependency<bool, bool>(
            consequentName,
            antecedentName,
            [](bool B, bool Acur) {
                // ¬B ⇒ ¬A (if B is false, force A false)
                return (!B) ? false : Acur;
            });
    }

    // Bool implication with explicit truth values:
    // (A == aVal) ⇒ (B = bVal), enforced from either side
    //
    // Enforcement behavior:
    // - If A becomes aVal, force B to bVal.
    // - If B becomes not bVal, force A to not aVal.
    void addImplicationConstraint(const std::string& AName, bool aVal,
        const std::string& BName, bool bVal)
    {
        registerDependency<bool, bool>(
            AName,
            BName,
            [aVal, bVal](bool A, bool Bcur) {
                // (A == aVal) ⇒ (B := bVal)
                return (A == aVal) ? bVal : Bcur;
            });

        registerDependency<bool, bool>(
            BName,
            AName,
            [aVal, bVal](bool B, bool Acur) {
                // ¬(B == bVal) ⇒ ¬(A == aVal)
                // i.e. if B != bVal, force A != aVal
                return (B != bVal) ? (!aVal) : Acur;
            });
    }

    // -----------------------------------------------------------------------------
    // Equivalence: A ⇔ B  (same-typed settings; changes propagate both ways)
    //
    // Logical relation: (A == B) as an invariant.
    // Implementation: mirror changes via observers, avoiding infinite loops via != check.
    template<typename T>
    void addEquivalence(const std::string& nameA, const std::string& nameB)
    {
        // A -> B
        {
            auto sub = addObserver<T>(nameA,
                [this, nameB](const T& newVal) {
                    auto getterB = getSettingGetter<T>(nameB);
                    T curB = getterB();
                    if (curB != newVal) {
                        getSettingSetter<T>(nameB)(newVal);
                    }
                });
            m_dependencySubscriptions.push_back(std::move(sub));
        }

        // B -> A
        {
            auto sub = addObserver<T>(nameB,
                [this, nameA](const T& newVal) {
                    auto getterA = getSettingGetter<T>(nameA);
                    T curA = getterA();
                    if (curA != newVal) {
                        getSettingSetter<T>(nameA)(newVal);
                    }
                });
            m_dependencySubscriptions.push_back(std::move(sub));
        }
    }

    // Exclusion: ¬(A ∧ B)  (mutually exclusive)
    // If either becomes true, force the other false.
    // Unline XOR, this allows both to be false.
    void addExclusion(const std::string& nameA, const std::string& nameB)
    {
        // A ⇒ ¬B
        registerDependency<bool, bool>(
            nameA,
            nameB,
            [](bool A, bool Bcur) {
                // if A then not B
                return A ? false : Bcur;
            });

        // B ⇒ ¬A
        registerDependency<bool, bool>(
            nameB,
            nameA,
            [](bool B, bool Acur) {
                // if B then not A
                return B ? false : Acur;
            });
    }

    // XOR (exactly one true): (A ⊕ B)
    // Equivalent to: (A ∨ B) ∧ ¬(A ∧ B)
    //
    // - If user turns one ON, force the other OFF.  (exclusion)
    // - If user turns one OFF and that would make both OFF, force the other ON.
    void addExclusiveOr(const std::string& nameA, const std::string& nameB)
    {
        // First enforce ¬(A ∧ B)
        addExclusion(nameA, nameB);

        // Now enforce (A ∨ B)
        // If A becomes false while B is false -> force B true
        {
            auto sub = addObserver<bool>(nameA,
                [this, nameB](bool Anew) {
                    if (Anew) return; // OR already satisfied
                    auto getB = getSettingGetter<bool>(nameB);
                    if (!getB()) {
                        getSettingSetter<bool>(nameB)(true);
                    }
                });
            m_dependencySubscriptions.push_back(std::move(sub));
        }

        // If B becomes false while A is false -> force A true
        {
            auto sub = addObserver<bool>(nameB,
                [this, nameA](bool Bnew) {
                    if (Bnew) return; // OR already satisfied
                    auto getA = getSettingGetter<bool>(nameA);
                    if (!getA()) {
                        getSettingSetter<bool>(nameA)(true);
                    }
                });
            m_dependencySubscriptions.push_back(std::move(sub));
        }
    }

    //
    // Group constraints (bool): AtMostOne / ExactlyOne
    //

    // AtMostOneTrue: for a set S, enforce that no two are true.
    // Logical relation: for all i!=j, ¬(Si ∧ Sj)
    void addAtMostOneTrue(const std::vector<std::string>& names)
    {
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string& controller = names[i];
            auto sub = addObserver<bool>(controller,
                [this, names, controller](bool controllerVal) {
                    if (!controllerVal) return;
                    // controller became true -> force all others false
                    for (const auto& other : names) {
                        if (other == controller) continue;
                        auto getOther = getSettingGetter<bool>(other);
                        if (getOther()) {
                            getSettingSetter<bool>(other)(false);
                        }
                    }
                });
            m_dependencySubscriptions.push_back(std::move(sub));
        }
    }

    // ExactlyOneTrue: for a set S, enforce:
    // - AtMostOneTrue(S)
    // - AtLeastOneTrue(S)  (if user turns the last true off, pick a deterministic fallback)
    //
    // Fallback policy: if all become false, force names[0] true.
    void addExactlyOneTrue(const std::vector<std::string>& names)
    {
        if (names.empty()) return;

        addAtMostOneTrue(names);

        // Enforce "at least one" with a fallback.
        // Whenever any setting flips, if all are false -> force the first true.
        for (const auto& n : names) {
            auto sub = addObserver<bool>(n,
                [this, names](bool /*unused*/) {
                    bool anyTrue = false;
                    for (const auto& s : names) {
                        if (getSettingGetter<bool>(s)()) { anyTrue = true; break; }
                    }
                    if (!anyTrue) {
                        getSettingSetter<bool>(names[0])(true);
                    }
                });
            m_dependencySubscriptions.push_back(std::move(sub));
        }
    }

private:
    std::atomic_uint64_t m_revision{1};
    std::unordered_map<std::string, std::unique_ptr<ISetting>> settings;
    std::vector<Subscription> m_dependencySubscriptions;

    SettingsManager() = default;
    // Helper function to retrieve a setting by name
    ISetting& getSettingByName(const std::string& name) {
        if (settings.find(name) == settings.end()) {
            throw std::runtime_error("Setting not found: " + name);
        }
        return *settings[name];
    }
};

inline SettingsManager& SettingsManager::GetInstance() {
    static SettingsManager instance;
    return instance;
}
