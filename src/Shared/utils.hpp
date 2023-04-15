#pragma once
#include "make_builder.hpp"
#include <math.h>
#include "math.hpp"

// ***********
// OPTIONAL
// ***********

template<typename T>
struct Optional {
  private:
    bool m_some = false;
    T m_value;

  public:
    Optional() : m_some(false), m_value() {}
    Optional(T value) : m_some(true), m_value(value) {}

    void set_none() {
        m_some = false;
    }

    void set_value(T value) {
        m_value = value;
        m_some = true;
    }

    auto value() -> T {
        return m_value;
    }

    auto some() -> bool {
        return m_some;
    }
};

template<typename T>
Optional<T> Some(T value) {  //NOLINT
    return Optional<T>(value);
}

template<typename T>
Optional<T> None() {  //NOLINT
    return Optional<T>();
}

// ***********
// Eventful
// ***********

struct EventfulBase {
    enum Event { Assigned, Incremented, Decremented };
};

template<typename T>
struct Eventful: EventfulBase {
    using Callback = std::function<void(Event, T)>;
    
    T value;
    Callback on_event;
    bool enabled = true;
    

    Eventful(): value() {}
    Eventful(const T& init): value(init) {} 
    Eventful(const T& init, Callback event_callback): value(init), on_event(event_callback) {} 

    operator T() const {
        return value;
    }

    void silent_set(T new_value) {
        this->value=new_value;
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }
    
    auto can_emit() -> bool {
        return on_event && enabled;
    }

    void try_emit(Event event) {
        if(can_emit()) {
            disable();
            on_event(event, value);
            enable();
        }
    }


    auto operator=(const T& new_value) -> Eventful<T>& {
        this->value = new_value;
        try_emit(Event::Assigned);
        return *this;
    }

    auto operator=(const Eventful<T>& new_obj) -> Eventful<T>& {
        this->value = new_obj.value;
        this->on_event = new_obj.on_event;
        try_emit(Event::Assigned);
        return *this;
    }
    
    
    auto operator+ (const T& other ) -> Eventful<T> {
        Eventful<T> result;
        result.value = value + other;
        result.on_event = on_event;
        return result;
    }
    
    
    auto operator+= (const T& other_value) -> Eventful<T>& {
        value += other_value;
        try_emit(Event::Incremented);
        return *this;
    }
    
    auto operator-= (const T& other_value) -> Eventful<T>& {
        value -= other_value;
        try_emit(Event::Decremented);
        return *this;
    }

};

template<typename T>
MAKE_BUILDER(BuildEventful, Eventful<T>, on_event, value);


// ***********
// random_string
// ***********

std::string random_string( size_t length )
{
    auto randchar = []() -> char
    {
        const char charset[] =
        "abcdefghijklmnopqrstuvwxyz";
        const size_t max_index = (sizeof(charset) - 1);
        return charset[ rand() % max_index ];
    };
    std::string str(length,0);
    std::generate_n( str.begin(), length, randchar );
    return str;
}

// ********
// select an idx using cv
// ********
enum SelectionMode {
    MIDI,
    MIDI_WRAP,
    FRACTION,
};

unsigned int select_idx_by_cv(double cv, SelectionMode mode, unsigned int max_idx, double max_cv = 10.0) {
    switch(mode) {
        case SelectionMode::MIDI:
            return (unsigned int) clamp(std::round(cv * 12.0), 0.0 , max_idx);
        case SelectionMode::MIDI_WRAP:
            return (unsigned int)std::round(cv * 12.0) % (max_idx + 1);
        case SelectionMode::FRACTION:
            return (unsigned int)std::round( max_idx * clamp(cv, 0.0, max_cv) / max_cv );
    }
}