template<typename T>
struct Optional {
  private:
    bool m_some = false;
    T m_value;

  public:
    Optional() = default;
    explicit Optional(T value) : m_some(true), m_value(value) {}

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
Optional<T> Some(T value) { //NOLINT
    return Optional<T>(value);
}

template<typename T>
Optional<T> None() { //NOLINT
    return Optional<T>();
}
