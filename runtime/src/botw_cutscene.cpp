// Temporary caps for the v208 events known to be sensitive to high frame rates.
// Track object + generation, so unrelated or nested event destruction cannot
// release another event's cap. The user's saved target is never changed.
#include "ppc.h"
#include "botw_fps.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {
struct Event { uint32_t id; int limit; };
std::mutex event_mutex;
std::unordered_map<uint32_t, Event> events;
std::atomic<int> cap{0};
int classify(uint32_t address) {
    if (!address) return 0;
    char name[10]{};
    for (unsigned i=0; i<9; ++i) {
        name[i]=ld8(address+i);
        if (!name[i]) return 0;
    }
    if ((name[0]!='D' && name[0]!='d') || name[1]!='e' || name[2]!='m' ||
        name[3]!='o' || name[7]!='_') return 0;
    for (unsigned i : {4u,5u,6u,8u}) if(name[i]<'0' || name[i]>'9') return 0;
    const int number=(name[4]-'0')*1000+(name[5]-'0')*100+(name[6]-'0')*10+name[8]-'0';
    if(number==1030) return 60;
    if(number==3480 || number==3481 || number==1620 || number==1630 ||
       number==3340 || number==1690 || (number>=1100 && number<=1250 && number%10==0)) return 30;
    return 0;
}
void update() {
    int limit=0;
    for(const auto& [object,event] : events)
        if(!limit || event.limit<limit) limit=event.limit;
    cap.store(limit,std::memory_order_relaxed);
}
}
extern "C" void botw_cutscene_begin(uint32_t object,uint32_t id,uint32_t name) {
    if(!object) return;
    const int limit=classify(name);
    std::lock_guard<std::mutex> lock(event_mutex);
    if(limit) events[object]={id,limit}; else events.erase(object);
    update();
}
extern "C" void botw_cutscene_end(uint32_t object,uint32_t id) {
    std::lock_guard<std::mutex> lock(event_mutex);
    auto it=events.find(object);
    if(it!=events.end() && it->second.id==id) { events.erase(it); update(); }
}
extern "C" int botw_cutscene_limit() { return cap.load(std::memory_order_relaxed); }
extern "C" int botw_effective_fps(int requested) {
    const int limit=botw_cutscene_limit();
    return limit && (requested==0 || requested>limit) ? limit : requested;
}
