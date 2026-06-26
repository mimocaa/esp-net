#ifndef SINGLETON_HPP
#define SINGLETON_HPP

template<class T>
class Singleton {
protected:
    Singleton() = default;
    virtual ~Singleton() = default;

public:
    Singleton(Singleton&)  = delete;
    Singleton(Singleton&&) = delete;
    auto operator= (Singleton&)  = delete;
    auto operator= (Singleton&&) = delete;

    static auto instance() -> T& {
        static T __instance__;
        return __instance__;
    }
};

#endif
