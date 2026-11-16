#include "vendor_services.h"
#include "../../../state/build_data/items/item_catalog.h"
#include "../../../state/build_data/vendors/service_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../core/logging/log.h"
#include <algorithm>
#include <cstring>

namespace dawn::client::content::vendors {
bool build_services(const middleware::content::packages::reader::Source& source,
    middleware::content::packages::reader::Scratch& scratch) noexcept {
    namespace domain=state::build_data::vendors::services;
    namespace reader=middleware::content::packages::reader;
    namespace tables=middleware::content::packages::tables;
    if(domain::ready()) {return true;}
    if(!state::build_data::item_definitions_ready()) {return false;}
    // Load the installed service catalog, including destination vendors. Actor
    // placement is independent: reading a vendor does not activate its NPC.
    std::vector<std::byte> index;std::uint32_t cls{};tables::Array rows{};
    if(!reader::read_tag(source,scratch,0x8131931DU,index,cls) || cls!=0x8080784AU
        || !tables::find_array_at(index,8,rows) || rows.elementClass!=0x8080784EU || rows.count>512) {return false;}
    std::vector<tables::IndexRow> vendors(rows.count);
    for(std::size_t i=0;i<vendors.size();++i) if(!tables::index_row(index,rows,i,vendors[i])) {return false;}
    std::vector<std::byte> items,definition;tables::Array itemRows{};
    if(!reader::read_tag(source,scratch,0x81327CCBU,items,cls)
        || !tables::find_array_at(items,8,itemRows)
        || itemRows.count>state::build_data::items::kDefinitionCapacity) {return false;}
    std::vector<domain::Loot> loot;
    std::vector<domain::QuestStep> quests;
    std::vector<domain::ItemFlag> itemFlags;
    std::vector<domain::Pursuit> pursuits;
    for(std::size_t i=0;i<itemRows.count;++i) {
        state::build_data::items::Definition item{};
        if(!state::build_data::find_item_definition_index(static_cast<std::uint16_t>(i),item)
            || (item.bucketId!=31 && item.bucketId!=37 && item.bucketId!=40)) {continue;}
        tables::IndexRow row{};
        if(!tables::index_row(items,itemRows,i,row) || !reader::read_tag(source,scratch,row.targetTag,definition,cls)
            || cls!=0x80807BEAU || definition.size()<164) {return false;}
        std::int64_t supplies{};std::memcpy(&supplies,definition.data()+144,8);
        if(supplies) {
            if(supplies<4-144 || supplies>static_cast<std::int64_t>(definition.size())-192) {return false;}
            const auto at=static_cast<std::size_t>(144+supplies);std::uint32_t type{};std::uint64_t count{};
            std::memcpy(&type,definition.data()+at-4,4);std::memcpy(&count,definition.data()+at,8);
            if(type!=0x808077ABU || count>256) {return false;}
            if(count) {
                tables::Array flags{};
                if(!tables::find_array_at(definition,at,flags) || flags.elementClass!=0x80807D4BU
                    || flags.dataOffset>definition.size() || count>(definition.size()-flags.dataOffset)/2) {return false;}
                for(std::size_t f=0;f<count;++f) {
                    std::uint16_t slot{};std::memcpy(&slot,definition.data()+flags.dataOffset+f*2,2);
                    if(slot!=UINT16_MAX) {itemFlags.push_back({static_cast<std::uint16_t>(i),slot});}
                }
            }
        }
        if(item.bucketId!=31) {
            domain::Pursuit progress{};
            if(!domain::read_pursuit(definition,static_cast<std::uint16_t>(i),progress)) {return false;}
            if(!progress.objectives.empty()) {pursuits.push_back(std::move(progress));}
            // Quest sets include legacy root definitions in bucket 37. A root
            // supplies the tracking slot and exact value for each owned step.
            std::int64_t relative{};std::memcpy(&relative,definition.data()+96,8);
            if(!relative) {continue;}
            if(relative<4-96 || relative>static_cast<std::int64_t>(definition.size())-128) {return false;}
            const auto at=static_cast<std::size_t>(96+relative);std::uint32_t type{};
            std::memcpy(&type,definition.data()+at-4,4);tables::Array steps{};
            if(type!=0x808077C8U || !tables::find_array_at(definition,at,steps)
                || steps.elementClass!=0x808077CAU || steps.count>256
                || steps.dataOffset>definition.size() || steps.count>(definition.size()-steps.dataOffset)/8) {return false;}
            std::uint16_t slot{};std::memcpy(&slot,definition.data()+at+16,2);
            if(slot==UINT16_MAX) {continue;}
            for(std::size_t s=0;s<steps.count;++s) {
                domain::QuestStep step{};step.slot=slot;step.ordinal=static_cast<std::uint16_t>(s);
                std::memcpy(&step.value,definition.data()+steps.dataOffset+s*8,4);
                std::memcpy(&step.item,definition.data()+steps.dataOffset+s*8+4,2);
                if(step.item!=UINT16_MAX) {quests.push_back(step);}
            }
            continue;
        }
        std::uint32_t preview{};std::memcpy(&preview,definition.data()+160,4);
        // Prime/world drops without a preview use the world equipment pool. These
        // fallback joins are server loot policy; authored previews take priority.
        const bool hasPreview=std::any_of(vendors.begin(),vendors.end(),[&](const auto& v){return v.definitionHash==preview;});
        if(!hasPreview) {
            if(i==6017 || i==6018 || i==6024 || i==12368) {preview=3163810067U;}
            if(i==6016 && vendors.size()>199) {preview=vendors[199].definitionHash;}
        }
        for(std::size_t v=0;v<vendors.size();++v) if(vendors[v].definitionHash==preview) {
            loot.push_back({static_cast<std::uint16_t>(i),static_cast<std::uint16_t>(v)});break;
        }
    }
    std::vector<domain::Blob> blobs;
    for(std::uint64_t i=0;i<rows.count;++i) {
        const auto& row=vendors[i];
        domain::Blob blob{static_cast<std::uint16_t>(i),row.definitionHash,{}};
        if(!reader::read_tag(source,scratch,row.targetTag,blob.bytes,cls) || cls!=0x80807850U) {return false;}
        blobs.push_back(std::move(blob));
    }
    std::vector<std::byte> flags,values,pool,objectives;
    const bool ok=reader::read_tag(source,scratch,0x81319322U,flags,cls) && cls==0x80807D36U
        && reader::read_tag(source,scratch,0x81319320U,values,cls) && cls==0x80807C80U
        && reader::read_tag(source,scratch,0x81319324U,pool,cls) && cls==0x80807C49U
        && reader::read_tag(source,scratch,0x81319344U,objectives,cls) && cls==0x8080775BU
        && domain::publish_conditions(std::move(flags),std::move(values),std::move(pool))
        && domain::publish(std::move(blobs),std::move(loot),std::move(quests),std::move(itemFlags),std::move(pursuits),std::move(objectives));
    core::log::write(core::log::Channel::server,ok?core::log::Level::info:core::log::Level::warn,
        ok?"ev=vendor stage=catalog result=ready":"ev=vendor stage=catalog result=invalid");
    return ok;
}
}
