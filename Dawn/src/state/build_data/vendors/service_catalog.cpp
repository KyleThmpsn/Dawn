#include "service_catalog.h"
#include "../items/item_catalog.h"
#include "../table.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include <cstring>
#include <array>
#include <memory>
#include <algorithm>

namespace dawn::state::build_data::vendors::services {
namespace {
Lock lock;
std::vector<Blob> blobs;
std::vector<Loot> loot;
std::vector<QuestStep> quests;
std::vector<ItemFlag> itemFlags;
std::vector<Pursuit> pursuits;
std::vector<Objective> objectives;
std::vector<std::uint16_t> manualFlags;
std::array<Binding,23500> flagBindings;
std::array<Binding,15500> valueBindings;
std::vector<Expression> pool;
namespace tables=middleware::content::packages::tables;
template<class T> bool read(std::span<const std::byte> bytes,std::size_t offset,T& out) noexcept {
    if(offset>bytes.size() || sizeof out>bytes.size()-offset) {return false;}
    std::memcpy(&out,bytes.data()+offset,sizeof out);return true;
}
bool array(std::span<const std::byte> bytes,std::size_t at,std::size_t stride,
    std::uint32_t cls,tables::Array& out) noexcept {
    out={};std::uint64_t count{};
    if(!read(bytes,at,count) || count>65535) {return false;}
    if(!count) {return true;}
    return tables::find_array_at(bytes,at,out) && out.count==count && out.elementClass==cls
        && out.dataOffset<=bytes.size() && count<=(bytes.size()-out.dataOffset)/stride;
}
const Blob* find(std::uint16_t vendor) noexcept {
    for(const auto& blob:blobs) if(blob.index==vendor) {return &blob;}return nullptr;
}
bool expression(std::span<const std::byte> bytes,std::size_t at,Expression& out) {
    tables::Array ops{};
    if(!array(bytes,at,8,0x80807D31U,ops) || ops.count>256) {return false;}
    out.resize(ops.count);
    for(std::size_t i=0;i<out.size();++i) if(!read(bytes,ops.dataOffset+i*8,out[i])) {return false;}
    return true;
}
bool parse(const Blob& blob,std::uint16_t sale,Offer& out) {
    tables::Array rows{},costs{};const auto bytes=std::span(blob.bytes);
    if(!array(bytes,48,184,0x80807861U,rows) || sale>=rows.count) {return false;}
    const auto at=rows.dataOffset+sale*184;
    out={};out.vendor=blob.index;out.sale=sale;
    if(!read(bytes,at+70,out.item) || !read(bytes,at+76,out.quantity)
        || !read(bytes,at+100,out.category) || out.quantity<=0
        || !array(bytes,at+32,48,0x80807865U,costs) || costs.count>16) {return false;}
    for(std::size_t i=0;i<costs.count;++i) {
        Cost cost{};const auto pos=costs.dataOffset+i*48;
        if(!read(bytes,pos,cost.item) || !read(bytes,pos+4,cost.quantity) || cost.quantity<0) {return false;}
        out.costs.push_back(cost);
    }
    for(const auto offset:{8U,120U}) {
        tables::Array conditions{};
        if(!array(bytes,at+offset,16,0x80807D2FU,conditions) || conditions.count>16) {return false;}
        for(std::size_t i=0;i<conditions.count;++i) {
            Expression condition;
            if(!expression(bytes,conditions.dataOffset+i*16,condition)) {return false;}
            out.conditions.push_back(std::move(condition));
        }
    }
    return true;
}
}
bool ready() noexcept {const Lock::Shared guard(lock);return !blobs.empty() && !pool.empty();}
bool publish(std::vector<Blob>&& candidate,std::vector<Loot> rewards,std::vector<QuestStep> steps,std::vector<ItemFlag> supplied,
    std::vector<Pursuit> progress,std::vector<std::byte> objectiveBytes) noexcept {
    if(candidate.empty() || candidate.size()>512) {return false;}
    for(std::size_t i=0;i<candidate.size();++i) {
        const auto& blob=candidate[i];tables::Array rows{};
        if(!blob.hash || blob.index>=512 || blob.bytes.size()>4*1024*1024
            || !array(blob.bytes,48,184,0x80807861U,rows)) {return false;}
        for(std::size_t j=0;j<i;++j) if(candidate[j].index==blob.index) {return false;}
        for(std::size_t row=0;row<rows.count;++row) {
            Offer checked;if(!parse(blob,static_cast<std::uint16_t>(row),checked)) {return false;}
        }
    }
    for(std::size_t i=0;i<rewards.size();++i) {
        if(rewards[i].item>=items::kDefinitionCapacity || std::none_of(candidate.begin(),candidate.end(),[&](const Blob& b){return b.index==rewards[i].vendor;})) {return false;}
        for(std::size_t j=0;j<i;++j) if(rewards[j].item==rewards[i].item) {return false;}
    }
    std::sort(steps.begin(),steps.end(),[](auto a,auto b){return a.item<b.item;});
    for(std::size_t i=0;i<steps.size();++i) {
        if(steps[i].item>=items::kDefinitionCapacity || steps[i].slot>=valueBindings.size()
            || (i && steps[i].item==steps[i-1].item
                && (steps[i].slot!=steps[i-1].slot || steps[i].value!=steps[i-1].value
                    || steps[i].ordinal!=steps[i-1].ordinal))) {return false;}
    }
    steps.erase(std::unique(steps.begin(),steps.end(),[](auto a,auto b){return a.item==b.item;}),steps.end());
    for(const auto f:supplied) if(f.item>=items::kDefinitionCapacity || f.slot>=flagBindings.size()) {return false;}
    std::sort(supplied.begin(),supplied.end(),[](auto a,auto b){return a.item<b.item || (a.item==b.item && a.slot<b.slot);});
    std::vector<Objective> parsed;
    if(!objectiveBytes.empty()) {
        tables::Array rows{};
        if(!array(objectiveBytes,8,160,0x8080775FU,rows) || rows.count>16384) {return false;}
        parsed.resize(rows.count);
        for(std::size_t i=0;i<rows.count;++i) {
            const auto at=rows.dataOffset+i*160;auto& o=parsed[i];tables::Array flags{};
            if(!read(objectiveBytes,at+48,o.threshold) || !expression(objectiveBytes,at+8,o.expression)
                || !array(objectiveBytes,at+72,2,0x80807D4BU,flags) || flags.count>256) {return false;}
            o.flags.resize(flags.count);
            for(std::size_t j=0;j<flags.count;++j) if(!read(objectiveBytes,flags.dataOffset+j*2,o.flags[j])) {return false;}
        }
    }
    std::sort(progress.begin(),progress.end(),[](const auto& a,const auto& b){return a.item<b.item;});
    for(std::size_t i=0;i<progress.size();++i) {
        if(progress[i].item>=items::kDefinitionCapacity || (i && progress[i-1].item==progress[i].item)) {return false;}
        for(const auto index:progress[i].objectives) if(index>=parsed.size()) {return false;}
    }
    std::vector<std::uint16_t> handIns;
    for(const auto& blob:candidate) {
        tables::Array interactions{};
        if(!array(blob.bytes,80,80,0x80807857U,interactions)) {return false;}
        for(std::size_t i=0;i<interactions.count;++i) {
            Expression condition;
            if(!expression(blob.bytes,interactions.dataOffset+i*80+8,condition)) {return false;}
            for(std::size_t j=0;j<condition.size();++j) if(condition[j].opcode==1
                && condition[j].operand<23500 && (j+1==condition.size() || condition[j+1].opcode!=2)) {
                handIns.push_back(static_cast<std::uint16_t>(condition[j].operand));
            }
        }
    }
    std::sort(handIns.begin(),handIns.end());handIns.erase(std::unique(handIns.begin(),handIns.end()),handIns.end());
    const Lock::Exclusive guard(lock);blobs=std::move(candidate);loot=std::move(rewards);quests=std::move(steps);
    itemFlags=std::move(supplied);pursuits=std::move(progress);objectives=std::move(parsed);manualFlags=std::move(handIns);return true;
}
bool read_pursuit(std::span<const std::byte> bytes,std::uint16_t index,Pursuit& out) noexcept {
    out={};out.item=index;std::int64_t relative{};
    if(!read(bytes,96,relative)) {return false;}
    if(relative) {
        if(relative<4-96 || relative>static_cast<std::int64_t>(bytes.size())-128) {return false;}
        std::uint32_t cls{},type{};const auto at=static_cast<std::size_t>(96+relative);
        if(!read(bytes,at-4,cls) || cls!=0x808077C8U || !read(bytes,at+20,type)) {return false;}
        out.globalQuest=type==0xCB72FC5DU; // Authored quest_global set type.
    }
    if(!read(bytes,48,relative)) {return false;}
    if(!relative) {return true;}
    if(relative<4-48 || relative>static_cast<std::int64_t>(bytes.size())-80) {return false;}
    const auto at=static_cast<std::size_t>(48+relative);std::uint32_t cls{};tables::Array rows{};std::uint8_t all{};
    if(!read(bytes,at-4,cls) || cls!=0x808077EBU || !read(bytes,at+16,out.site) || !read(bytes,at+23,all)
        || all>1 || !array(bytes,at,2,0x808087B1U,rows) || rows.count>32) {return false;}
    out.requireAll=all!=0;out.objectives.resize(rows.count);
    for(std::size_t i=0;i<rows.count;++i) if(!read(bytes,rows.dataOffset+i*2,out.objectives[i])) {return false;}
    return true;
}
bool next_step(const QuestStep& step,QuestStep& out) noexcept {
    const Lock::Shared guard(lock);
    for(const auto q:quests) if(q.slot==step.slot && q.ordinal==step.ordinal+1) {out=q;return true;}
    return false;
}
bool pursuit(std::uint16_t item,Pursuit& out) noexcept {
    const Lock::Shared guard(lock);
    const auto it=std::lower_bound(pursuits.begin(),pursuits.end(),item,[](const auto& q,auto i){return q.item<i;});
    if(it==pursuits.end() || it->item!=item) {return false;}out=*it;return true;
}
bool objective(std::uint16_t index,Objective& out) noexcept {
    const Lock::Shared guard(lock);if(index>=objectives.size()) {return false;}out=objectives[index];return true;
}
bool hand_in(const Pursuit& p,const Interaction& interaction) noexcept {
    for(const auto index:p.objectives) {
        Objective o{};if(!objective(index,o)) {return false;}
        for(const auto flag:o.flags) for(std::size_t i=0;i<interaction.condition.size();++i) {
            const auto op=interaction.condition[i];
            if(op.opcode==1 && op.operand==flag
                && (i+1==interaction.condition.size() || interaction.condition[i+1].opcode!=2)) {return true;}
        }
    }
    return false;
}
bool manual(const Pursuit& p) noexcept {
    const Lock::Shared guard(lock);
    for(const auto index:p.objectives) if(index<objectives.size()) for(const auto flag:objectives[index].flags)
        if(std::binary_search(manualFlags.begin(),manualFlags.end(),flag)) {return true;}
    return false;
}
bool item_flag(std::uint16_t item,std::uint16_t slot) noexcept {
    const Lock::Shared guard(lock);
    auto it=std::lower_bound(itemFlags.begin(),itemFlags.end(),item,[](auto f,auto i){return f.item<i;});
    for(;it!=itemFlags.end() && it->item==item;++it) if(it->slot==slot) {return true;}
    return false;
}
bool quest_step(std::uint16_t item,QuestStep& out) noexcept {
    const Lock::Shared guard(lock);
    const auto it=std::lower_bound(quests.begin(),quests.end(),item,[](auto q,auto i){return q.item<i;});
    if(it==quests.end() || it->item!=item) {return false;}out=*it;return true;
}
bool loot_vendor(std::uint16_t item,std::uint16_t& vendor) noexcept {
    const Lock::Shared guard(lock);
    for(const auto& row:loot) if(row.item==item) {vendor=row.vendor;return true;}
    return false;
}
bool offer(std::uint16_t vendor,std::uint16_t sale,Offer& out) noexcept {
    const Lock::Shared guard(lock);const auto* blob=find(vendor);return blob && parse(*blob,sale,out);
}
bool interaction(std::uint16_t vendor,std::uint16_t index,Interaction& out) noexcept {
    const Lock::Shared guard(lock);const auto* blob=find(vendor);tables::Array rows{};out={};
    if(!blob || !array(blob->bytes,80,80,0x80807857U,rows) || index>=rows.count) {return false;}
    const auto at=rows.dataOffset+index*80;
    tables::Array replies{};
    if(!read(blob->bytes,at,out.item) || !read(blob->bytes,at+4,out.presentation) || !read(blob->bytes,at+56,out.category)
        || !expression(blob->bytes,at+8,out.condition)
        || !array(blob->bytes,at+40,24,0x8080785BU,replies) || replies.count>32) {return false;}
    out.replies.resize(replies.count);
    for(std::size_t i=0;i<replies.count;++i) {
        auto& reply=out.replies[i];const auto pos=replies.dataOffset+i*24;
        if(!expression(blob->bytes,pos,reply.condition) || !read(blob->bytes,pos+16,reply.selection)
            || reply.selection>1 || !read(blob->bytes,pos+18,reply.site)) {return false;}
    }
    return true;
}
bool reward_items(std::uint16_t vendor,std::vector<std::uint16_t>& out) noexcept {
    const Lock::Shared guard(lock);const auto* blob=find(vendor);tables::Array rows{};out.clear();
    if(!blob || !array(blob->bytes,48,184,0x80807861U,rows)) {return false;}
    for(std::size_t i=0;i<rows.count;++i) {
        std::uint16_t item{};if(!read(blob->bytes,rows.dataOffset+i*184+70,item)) {return false;}
        out.push_back(item);
    }
    return !out.empty();
}
bool publish_conditions(std::vector<std::byte> flags,std::vector<std::byte> values,std::vector<std::byte> source) noexcept {
    auto f=std::make_unique<decltype(flagBindings)>();auto v=std::make_unique<decltype(valueBindings)>();
    for(int value=0;value<2;++value) {
        const auto bytes=std::span(value?values:flags);const auto banks=value?4U:5U;
        for(unsigned bank=0;bank<banks;++bank) {
            tables::Array rows{};
            if(!array(bytes,8+bank*16,8,value?0x80807C8DU:0x80807D48U,rows)) {return false;}
            for(std::size_t i=0;i<rows.count;++i) {
                std::uint16_t slot{};
                if(!read(bytes,rows.dataOffset+i*8+4,slot) || slot>=(value?v->size():f->size())) {return false;}
                auto& target=value?(*v)[slot]:(*f)[slot];if(target.present) {return false;}
                target={static_cast<std::uint16_t>(i),static_cast<std::uint8_t>(bank),true};
            }
        }
    }
    tables::Array rows{};std::vector<Expression> parsed;
    if(!array(source,8,24,0x80807C4FU,rows) || rows.count!=6541) {return false;}
    parsed.resize(rows.count);
    for(std::size_t i=0;i<rows.count;++i) if(!expression(source,rows.dataOffset+i*24+8,parsed[i])) {return false;}
    const Lock::Exclusive guard(lock);flagBindings=*f;valueBindings=*v;pool=std::move(parsed);return true;
}
Binding binding(bool value,std::uint16_t slot) noexcept {
    const Lock::Shared guard(lock);
    if(value) {return slot<valueBindings.size()?valueBindings[slot]:Binding{};}
    return slot<flagBindings.size()?flagBindings[slot]:Binding{};
}
bool pooled(std::uint16_t index,Expression& out) noexcept {
    const Lock::Shared guard(lock);if(index>=pool.size()) {return false;}out=pool[index];return true;
}
}
