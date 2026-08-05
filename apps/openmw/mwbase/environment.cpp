#include "environment.hpp"

#include <cassert>

#include <components/resource/resourcesystem.hpp>

MWBase::Environment* MWBase::Environment::sThis = nullptr;

#ifdef __vita__
thread_local float MWBase::Environment::sVitaDtScale = 1.f;
#endif

MWBase::Environment::Environment()
{
    assert(sThis == nullptr);
    sThis = this;
}

MWBase::Environment::~Environment()
{
    sThis = nullptr;
}
