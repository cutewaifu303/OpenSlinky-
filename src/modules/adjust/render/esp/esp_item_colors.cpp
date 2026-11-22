#include "modules/adjust/render/esp/esp_item_colors.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "adjust/jni_lookup.hpp"
#include "minecraft/mappings.hpp"
#include "minecraft/mc_wrappers.hpp"
#include "modules/adjust/render/esp/adjust_render_jni.hpp"

namespace slinky::modules::adjust_esp {

namespace {

namespace J = adjust::jni;

// Verbatim ModernItemName data: "registry_name,b|i,metadata,rrggbb;" (b = block item, i = item).
constexpr const char* kData =
    "stone,b,-1,7c7c7c;stone,b,1,7c5444;stone,b,2,966656;stone,b,3,a4a4a4;stone,b,4,acacac;stone,b,5,8c8c8c;stone,b,6,8c9494;"
    "grass,b,-1,858585;dirt,b,-1,7c543c;dirt,b,1,79533b;dirt,b,2,50391b;cobblestone,b,-1,8c848c;planks,b,-1,68562d;planks,b,1,74542c;"
    "planks,b,2,9d8d64;planks,b,3,b68461;planks,b,4,8c452c;planks,b,5,2d1c0c;sapling,b,-1,328d32;sapling,b,1,315131;sapling,b,2,d8d8b0;"
    "sapling,b,3,2c6c1c;sapling,b,4,755510;sapling,b,5,165916;bedrock,b,-1,646464;sand,b,-1,e4dcb4;sand,b,1,cc6c24;gravel,b,-1,847c7c;"
    "gold_ore,b,-1,c09833;iron_ore,b,-1,9d9d9d;coal_ore,b,-1,181818;log,b,-1,745c34;log,b,1,4d3515;log,b,2,dcdccc;log,b,3,5a441b;"
    "log2,b,-1,6c645c;log2,b,1,3c341c;leaves,b,-1,9c9c9c;leaves,b,1,8c8c8c;leaves,b,2,8c8484;leaves,b,3,9c9c9c;leaves2,b,-1,b4b4b4;"
    "leaves2,b,1,b4bcb4;sponge,b,-1,cccc4c;sponge,b,1,b4bc4c;glass,b,-1,d4ecec;lapis_ore,b,-1,5880e8;lapis_block,b,-1,204888;dispenser,b,-1,a2a2a2;"
    "sandstone,b,-1,ceb684;sandstone,b,1,dfdaae;sandstone,b,2,dfd7aa;noteblock,b,-1,5c3727;golden_rail,b,-1,6a6152;detector_rail,b,-1,6e706c;sticky_piston,b,-1,5ea44f;"
    "web,b,-1,e4ecec;tallgrass,b,-1,328d32;tallgrass,b,1,328d32;tallgrass,b,2,30281d;deadbush,b,-1,9e6c2e;piston,b,-1,c49c64;wool,b,-1,dcdfe0;"
    "wool,b,1,f87f1c;wool,b,2,b239a8;wool,b,3,3cb6db;wool,b,4,f9c324;wool,b,5,72bc1c;wool,b,6,e2729a;wool,b,7,3c4444;"
    "wool,b,8,828278;wool,b,9,14808c;wool,b,10,852db5;wool,b,11,3a3ca4;wool,b,12,7d4e2d;wool,b,13,52681d;wool,b,14,942424;"
    "wool,b,15,1a1c1d;yellow_flower,b,-1,f49c24;red_flower,b,-1,4c8c2c;red_flower,b,1,2cc4fc;red_flower,b,2,bc7cec;red_flower,b,3,d4ecec;red_flower,b,4,54ac2c;"
    "red_flower,b,5,549c2c;red_flower,b,6,f4f4f4;red_flower,b,7,ecc4fc;red_flower,b,8,f4bc24;brown_mushroom,b,-1,946c54;red_mushroom,b,-1,c41c24;gold_block,b,-1,fce448;"
    "iron_block,b,-1,e7e7e7;stone_slab,b,-1,777777;stone_slab,b,1,ceb684;stone_slab,b,2,fcf6f6;stone_slab,b,3,8c848c;stone_slab,b,4,7c3c2c;stone_slab,b,5,8c8c8c;"
    "stone_slab,b,6,371c1d;stone_slab,b,7,f4ecec;brick_block,b,-1,f4f4f4;tnt,b,-1,671e1e;bookshelf,b,-1,275564;mossy_cobblestone,b,-1,748454;obsidian,b,-1,3e2457;"
    "torch,b,-1,54442c;mob_spawner,b,-1,2c4454;oak_stairs,b,-1,4c5464;chest,b,-1,956a24;diamond_ore,b,-1,717171;diamond_block,b,-1,65f5ec;crafting_table,b,-1,8c8c8c;"
    "farmland,b,-1,946c4c;furnace,b,-1,8c8c8c;lit_furnace,b,-1,8c8c8c;ladder,b,-1,947444;rail,b,-1,a6a6a6;stone_stairs,b,-1,8c848c;lever,b,-1,747474;"
    "stone_pressure_plate,b,-1,7c7c7c;wooden_pressure_plate,b,-1,68562d;redstone_ore,b,-1,817f7f;redstone_torch,b,-1,fcd4d4;stone_button,b,-1,7c7c7c;snow_layer,b,-1,c2b692;snow_layer,b,1,c2b692;"
    "snow_layer,b,2,c2b692;snow_layer,b,3,c2b692;snow_layer,b,4,c2b692;snow_layer,b,5,c2b692;snow_layer,b,6,c2b692;snow_layer,b,7,c2b692;ice,b,-1,94bcfc;"
    "snow,b,-1,79523d;cactus,b,-1,649c34;clay,b,-1,acacbc;jukebox,b,-1,5c3727;fence,b,-1,68562d;spruce_fence,b,-1,74542c;birch_fence,b,-1,9d8d64;"
    "jungle_fence,b,-1,b68461;dark_oak_fence,b,-1,2d1c0c;acacia_fence,b,-1,8c452c;pumpkin,b,-1,401104;netherrack,b,-1,642c2c;soul_sand,b,-1,5c443c;glowstone,b,-1,fcdc74;"
    "lit_pumpkin,b,-1,c57814;trapdoor,b,-1,a2804e;monster_egg,b,-1,7c7c7c;monster_egg,b,1,8c848c;monster_egg,b,2,8c8c8c;monster_egg,b,3,777577;monster_egg,b,4,7c787c;"
    "monster_egg,b,5,606060;stonebrick,b,-1,8c8c8c;stonebrick,b,1,777577;stonebrick,b,2,7c787c;stonebrick,b,3,606060;brown_mushroom_block,b,-1,947454;red_mushroom_block,b,-1,fc9c9c;"
    "iron_bars,b,-1,71766c;glass_pane,b,-1,d4ecec;melon_block,b,-1,bc3424;vine,b,-1,6c6c6c;fence_gate,b,-1,68562d;spruce_fence_gate,b,-1,74542c;birch_fence_gate,b,-1,9d8d64;"
    "jungle_fence_gate,b,-1,b68461;dark_oak_fence_gate,b,-1,2d1c0c;acacia_fence_gate,b,-1,8c452c;brick_stairs,b,-1,7c3c2c;stone_brick_stairs,b,-1,8c8c8c;mycelium,b,-1,716164;waterlily,b,-1,949494;"
    "nether_brick,b,-1,371c1d;nether_brick_fence,b,-1,4e2a2e;nether_brick_stairs,b,-1,371c1d;enchanting_table,b,-1,a6a29b;end_portal_frame,b,-1,bcac74;end_stone,b,-1,dce4a4;dragon_egg,b,-1,0c0c0c;"
    "redstone_lamp,b,-1,844c2c;wooden_slab,b,-1,4c5464;wooden_slab,b,1,6c5832;wooden_slab,b,2,cbb87d;wooden_slab,b,3,947c44;wooden_slab,b,4,4c5464;wooden_slab,b,5,493924;"
    "sandstone_stairs,b,-1,ceb684;emerald_ore,b,-1,14b049;ender_chest,b,-1,132026;tripwire_hook,b,-1,b4945c;emerald_block,b,-1,14b049;spruce_stairs,b,-1,6c5832;birch_stairs,b,-1,cbb87d;"
    "jungle_stairs,b,-1,947c44;command_block,b,-1,bd7d55;beacon,b,-1,c1f1ee;cobblestone_wall,b,-1,8c848c;cobblestone_wall,b,1,748454;wooden_button,b,-1,68562d;anvil,b,-1,444444;"
    "anvil,b,1,4c4c4c;anvil,b,2,595959;trapped_chest,b,-1,956a24;light_weighted_pressure_plate,b,-1,e7e7e7;heavy_weighted_pressure_plate,b,-1,fce448;daylight_detector,b,-1,3c3424;redstone_block,b,-1,bc240c;"
    "quartz_ore,b,-1,5a2924;hopper,b,-1,4c4c4c;quartz_block,b,-1,f4ecec;quartz_block,b,1,f4ecec;quartz_block,b,2,ece4dc;quartz_stairs,b,-1,f4ecec;activator_rail,b,-1,aaa294;"
    "dropper,b,-1,a2a2a2;stained_hardened_clay,b,-1,d4b4a4;stained_hardened_clay,b,1,a45424;stained_hardened_clay,b,2,9c5c6d;stained_hardened_clay,b,3,746c8c;stained_hardened_clay,b,4,bc8424;stained_hardened_clay,b,5,6c7434;"
    "stained_hardened_clay,b,6,a45452;stained_hardened_clay,b,7,3c2c24;stained_hardened_clay,b,8,8c6c64;stained_hardened_clay,b,9,5c5c5c;stained_hardened_clay,b,10,7c4c5a;stained_hardened_clay,b,11,4c3c5c;stained_hardened_clay,b,12,4c3424;"
    "stained_hardened_clay,b,13,4c542c;stained_hardened_clay,b,14,944434;stained_hardened_clay,b,15,241c14;barrier,b,-1,e40404;iron_trapdoor,b,-1,d4cccc;hay_block,b,-1,94841c;carpet,b,-1,dcdfe0;"
    "carpet,b,1,f87f1c;carpet,b,2,b239a8;carpet,b,3,3cb6db;carpet,b,4,f9c324;carpet,b,5,72bc1c;carpet,b,6,e2729a;carpet,b,7,3c4444;"
    "carpet,b,8,828278;carpet,b,9,14808c;carpet,b,10,852db5;carpet,b,11,3a3ca4;carpet,b,12,7d4e2d;carpet,b,13,52681d;carpet,b,14,942424;"
    "carpet,b,15,1a1c1d;hardened_clay,b,-1,9c5c44;coal_block,b,-1,141414;packed_ice,b,-1,a8c7fc;acacia_stairs,b,-1,4c5464;dark_oak_stairs,b,-1,493924;slime,b,-1,74c464;"
    "double_plant,b,-1,54ac2c;double_plant,b,1,9c649c;double_plant,b,2,747474;double_plant,b,3,6c6c6c;double_plant,b,4,244824;double_plant,b,5,dc94f4;stained_glass,b,-1,d4ecec;"
    "stained_glass,b,1,dc7c34;stained_glass,b,2,b44cdc;stained_glass,b,3,649cdc;stained_glass,b,4,e4e434;stained_glass,b,5,7ccc1c;stained_glass,b,6,f47ca4;stained_glass,b,7,4c4c4c;"
    "stained_glass,b,8,9c9c9c;stained_glass,b,9,4c7c9c;stained_glass,b,10,7c3cb4;stained_glass,b,11,344cb4;stained_glass,b,12,644c34;stained_glass,b,13,647c34;stained_glass,b,14,9c3434;"
    "stained_glass,b,15,1c1c1c;stained_glass_pane,b,-1,d4ecec;stained_glass_pane,b,1,dc7c34;stained_glass_pane,b,2,b44cdc;stained_glass_pane,b,3,649cdc;stained_glass_pane,b,4,e4e434;stained_glass_pane,b,5,7ccc1c;"
    "stained_glass_pane,b,6,f47ca4;stained_glass_pane,b,7,4c4c4c;stained_glass_pane,b,8,9c9c9c;stained_glass_pane,b,9,4c7c9c;stained_glass_pane,b,10,7c3cb4;stained_glass_pane,b,11,344cb4;stained_glass_pane,b,12,644c34;"
    "stained_glass_pane,b,13,647c34;stained_glass_pane,b,14,9c3434;stained_glass_pane,b,15,1c1c1c;prismarine,b,-1,68a3a0;prismarine,b,1,8cc8c0;prismarine,b,2,3c6b5b;sea_lantern,b,-1,d2e3da;"
    "red_sandstone,b,-1,a65111;red_sandstone,b,1,c66c24;red_sandstone,b,2,c76c24;red_sandstone_stairs,b,-1,a65111;stone_slab2,b,-1,a65111;iron_shovel,i,-1,dcdcdc;iron_pickaxe,i,-1,dcdcdc;"
    "iron_axe,i,-1,dcdcdc;flint_and_steel,i,-1,b4b4b4;apple,i,-1,b4141c;bow,i,-1,7a531b;arrow,i,-1,c4c4c4;coal,i,-1,221e24;coal,i,1,584c3c;"
    "diamond,i,-1,56f8da;iron_ingot,i,-1,d0cece;gold_ingot,i,-1,d5ae49;iron_sword,i,-1,bcbcbc;wooden_sword,i,-1,4c3414;wooden_shovel,i,-1,4c3414;wooden_pickaxe,i,-1,4c3414;"
    "wooden_axe,i,-1,4c3414;stone_sword,i,-1,b4b4ac;stone_shovel,i,-1,b4b4ac;stone_pickaxe,i,-1,b4b4ac;stone_axe,i,-1,b4b4ac;diamond_sword,i,-1,23cba7;diamond_shovel,i,-1,23cba7;"
    "diamond_pickaxe,i,-1,23cba7;diamond_axe,i,-1,23cba7;stick,i,-1,4c3414;bowl,i,-1,3c2808;mushroom_stew,i,-1,543c0c;golden_sword,i,-1,ecec54;golden_shovel,i,-1,ecec54;"
    "golden_pickaxe,i,-1,ecec54;golden_axe,i,-1,ecec54;string,i,-1,dcdcdc;feather,i,-1,d4d4d4;gunpowder,i,-1,808080;wooden_hoe,i,-1,745c24;stone_hoe,i,-1,8c8c8c;"
    "iron_hoe,i,-1,dcdcdc;diamond_hoe,i,-1,2cc4ac;golden_hoe,i,-1,ecec54;wheat_seeds,i,-1,6cb43c;wheat,i,-1,a49454;bread,i,-1,644c14;leather_helmet,i,-1,745c24;"
    "leather_chestplate,i,-1,745c24;leather_leggings,i,-1,745c24;leather_boots,i,-1,745c24;chainmail_helmet,i,-1,d2d2d2;chainmail_chestplate,i,-1,d2d2d2;chainmail_leggings,i,-1,cccccc;chainmail_boots,i,-1,dcdcdc;"
    "iron_helmet,i,-1,c4c4c4;iron_chestplate,i,-1,c4c4c4;iron_leggings,i,-1,cccccc;iron_boots,i,-1,dcdcdc;diamond_helmet,i,-1,24c4b4;diamond_chestplate,i,-1,24c4b4;diamond_leggings,i,-1,69f1e1;"
    "diamond_boots,i,-1,a4fcec;golden_helmet,i,-1,ecd93e;golden_chestplate,i,-1,ecdc41;golden_leggings,i,-1,ecd034;golden_boots,i,-1,fcfc74;flint,i,-1,cccccc;porkchop,i,-1,f87474;"
    "cooked_porkchop,i,-1,5c4c24;painting,i,-1,94744c;golden_apple,i,-1,c09833;golden_apple,i,1,ecd84a;sign,i,-1,67502f;wooden_door,i,-1,9c844c;spruce_door,i,-1,7c5c34;"
    "birch_door,i,-1,e2e0dd;jungle_door,i,-1,b4845c;acacia_door,i,-1,925631;dark_oak_door,i,-1,54341c;bucket,i,-1,b6b6b6;water_bucket,i,-1,68b2b9;lava_bucket,i,-1,bcb0ac;"
    "minecart,i,-1,272f34;saddle,i,-1,7c3c1c;iron_door,i,-1,b4b4b4;redstone,i,-1,545454;snowball,i,-1,d4f4f4;boat,i,-1,6c5c3c;leather,i,-1,9c4c2c;"
    "milk_bucket,i,-1,b7bcbd;brick,i,-1,7c3c2c;clay_ball,i,-1,a4a4b4;reeds,i,-1,64a42c;paper,i,-1,acacac;book,i,-1,644c14;slime_ball,i,-1,86ce7e;"
    "chest_minecart,i,-1,3d392d;furnace_minecart,i,-1,9f9fa0;egg,i,-1,d9c2a3;compass,i,-1,bcbcbc;fishing_rod,i,-1,4c3414;clock,i,-1,9e510e;glowstone_dust,i,-1,744822;"
    "fish,i,-1,c4a474;fish,i,1,bc5e5c;fish,i,2,ecbca4;fish,i,3,1c2c24;cooked_fish,i,-1,d4c4ac;cooked_fish,i,1,d9865e;dye,i,-1,7c6474;"
    "dye,i,1,8c243c;dye,i,2,4c6c1c;dye,i,3,4c2c14;dye,i,4,1c54ac;dye,i,5,a454cc;dye,i,6,2c7c9c;dye,i,7,bcbcc4;"
    "dye,i,8,848484;dye,i,9,e69cc4;dye,i,10,86ce21;dye,i,11,b4941c;dye,i,12,1c4474;dye,i,13,ac4cac;dye,i,14,f4b43c;"
    "dye,i,15,d4d4dc;bone,i,-1,fcfcec;sugar,i,-1,bcbccc;cake,i,-1,d4ccac;bed,i,-1,942424;repeater,i,-1,840404;cookie,i,-1,bc7434;"
    "filled_map,i,-1,a4ac4c;shears,i,-1,bda69b;melon,i,-1,328d32;pumpkin_seeds,i,-1,ccb46c;melon_seeds,i,-1,645434;beef,i,-1,e43c34;cooked_beef,i,-1,965941;"
    "chicken,i,-1,dcac94;cooked_chicken,i,-1,cc7c4c;mutton,i,-1,dc3c34;cooked_mutton,i,-1,542c1c;rabbit,i,-1,dca48c;cooked_rabbit,i,-1,612e15;rabbit_stew,i,-1,744c0c;"
    "rabbit_foot,i,-1,c49c64;rabbit_hide,i,-1,846444;rotten_flesh,i,-1,84441c;ender_pearl,i,-1,248474;blaze_rod,i,-1,dcac24;ghast_tear,i,-1,ccf4f4;gold_nugget,i,-1,c09833;"
    "nether_wart,i,-1,a42434;potion,i,-1,b4ccec;glass_bottle,i,-1,b4ccec;spider_eye,i,-1,7c0c24;fermented_spider_eye,i,-1,9c1c2c;blaze_powder,i,-1,fce41c;magma_cream,i,-1,c9691e;"
    "brewing_stand,i,-1,646464;cauldron,i,-1,4c4c4c;ender_eye,i,-1,1c4129;speckled_melon,i,-1,dcacac;spawn_egg,i,-1,cccccc;spawn_egg,i,50,cccccc;spawn_egg,i,51,cccccc;"
    "spawn_egg,i,52,cccccc;spawn_egg,i,54,cccccc;spawn_egg,i,55,cccccc;spawn_egg,i,56,cccccc;spawn_egg,i,57,cccccc;spawn_egg,i,58,cccccc;spawn_egg,i,59,cccccc;"
    "spawn_egg,i,60,cccccc;spawn_egg,i,61,cccccc;spawn_egg,i,62,cccccc;spawn_egg,i,65,cccccc;spawn_egg,i,66,cccccc;spawn_egg,i,67,cccccc;spawn_egg,i,68,cccccc;"
    "spawn_egg,i,90,cccccc;spawn_egg,i,91,cccccc;spawn_egg,i,92,cccccc;spawn_egg,i,93,cccccc;spawn_egg,i,94,cccccc;spawn_egg,i,95,cccccc;spawn_egg,i,96,cccccc;"
    "spawn_egg,i,98,cccccc;spawn_egg,i,100,cccccc;spawn_egg,i,101,cccccc;spawn_egg,i,120,cccccc;experience_bottle,i,-1,dce45c;fire_charge,i,-1,b05008;writable_book,i,-1,3d1706;"
    "written_book,i,-1,74341c;emerald,i,-1,14b049;item_frame,i,-1,a7945d;flower_pot,i,-1,9c6454;carrot,i,-1,fc9c2c;potato,i,-1,ecc06c;baked_potato,i,-1,8c4904;"
    "poisonous_potato,i,-1,b68c44;map,i,-1,d9c2a3;golden_carrot,i,-1,c09833;skull,i,-1,b6926c;skull,i,1,181818;skull,i,2,376c3b;skull,i,3,e4e4e4;"
    "skull,i,4,328d32;carrot_on_a_stick,i,-1,cc7c13;nether_star,i,-1,dce1c6;pumpkin_pie,i,-1,b1642c;fireworks,i,-1,bc2c2c;firework_charge,i,-1,747474;enchanted_book,i,-1,45250e;"
    "comparator,i,-1,b0b4ac;netherbrick,i,-1,371c1d;quartz,i,-1,f4ecec;tnt_minecart,i,-1,c21d1d;hopper_minecart,i,-1,363838;armor_stand,i,-1,b4945c;iron_horse_armor,i,-1,3c3c3c;"
    "golden_horse_armor,i,-1,c09833;diamond_horse_armor,i,-1,1caca4;lead,i,-1,a49c94;name_tag,i,-1,e4c48c;command_block_minecart,i,-1,bda69b;record_13,i,-1,444444;record_cat,i,-1,1c1c1c;"
    "record_blocks,i,-1,545454;record_chirp,i,-1,545454;record_far,i,-1,545454;record_mall,i,-1,545454;record_mellohi,i,-1,444444;record_stal,i,-1,444444;record_strad,i,-1,444444;"
    "record_ward,i,-1,545454;record_11,i,-1,545454;record_wait,i,-1,1c1c1c;prismarine_shard,i,-1,549c84;prismarine_crystals,i,-1,b4dccc;banner,i,-1,1c1c24;banner,i,1,b42c24;"
    "banner,i,2,5c7c14;banner,i,3,845434;banner,i,4,3c44ac;banner,i,5,8c34bc;banner,i,6,149c9c;banner,i,7,9c9c94;banner,i,8,444c54;"
    "banner,i,9,f48cac;banner,i,10,84c41c;banner,i,11,fcdc3c;banner,i,12,3cb4dc;banner,i,13,c44cbc;banner,i,14,fc841c;banner,i,15,ececec"
    ;

struct Entry {
    int item_id = -1;
    int metadata = -1;
    adjust::Argb rgb = 0;
};

std::vector<Entry> g_entries;
bool g_resolved = false;

// Resolves the registry names to numeric item IDs once (Item.getItemFromBlock(Block.getBlockFromName(name)) for
// blocks, Item.getByNameOrId(name) for items), exactly like the static initializer of ModernItemName.
void resolve(events::JniContext* jni) noexcept {
    g_resolved = true;
    if (!adjust_render::available(jni)) {
        return;
    }
    jclass cls_block = jni->mappings->cls_block;
    jclass cls_item = jni->mappings->cls_item;
    const jmethodID m_block_from_name = J::static_method(jni, cls_block, {"b", "getBlockFromName", "func_149684_b"},
                                                         "(Ljava/lang/String;)Lafh;",
                                                         "(Ljava/lang/String;)Lnet/minecraft/block/Block;");
    const jmethodID m_item_from_block = J::static_method(jni, cls_item, {"a", "getItemFromBlock", "func_150898_a"},
                                                         "(Lafh;)Lzw;",
                                                         "(Lnet/minecraft/block/Block;)Lnet/minecraft/item/Item;");
    const jmethodID m_item_by_name = J::static_method(jni, cls_item, {"d", "getByNameOrId", "func_111206_d"},
                                                      "(Ljava/lang/String;)Lzw;",
                                                      "(Ljava/lang/String;)Lnet/minecraft/item/Item;");
    if (m_block_from_name == nullptr || m_item_from_block == nullptr || m_item_by_name == nullptr) {
        return;
    }

    JNIEnv* env = jni->env;
    const char* cursor = kData;
    while (*cursor != '\0') {
        const char* end = std::strchr(cursor, ';');
        const std::string row = end != nullptr ? std::string(cursor, end) : std::string(cursor);
        cursor = end != nullptr ? end + 1 : cursor + row.size();

        const size_t c1 = row.find(',');
        const size_t c2 = row.find(',', c1 + 1);
        const size_t c3 = row.find(',', c2 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos) {
            continue;
        }
        const std::string name = row.substr(0, c1);
        const bool is_block = row.compare(c1 + 1, c2 - c1 - 1, "b") == 0;
        Entry entry{};
        entry.metadata = std::atoi(row.substr(c2 + 1, c3 - c2 - 1).c_str());
        entry.rgb = 0xFF000000u | static_cast<adjust::Argb>(std::strtoul(row.substr(c3 + 1).c_str(), nullptr, 16));

        jstring jname = env->NewStringUTF(name.c_str());
        jobject item = nullptr;
        if (is_block) {
            jobject block = env->CallStaticObjectMethod(cls_block, m_block_from_name, jname);
            if (!J::clear_exception(jni) && block != nullptr) {
                item = env->CallStaticObjectMethod(cls_item, m_item_from_block, block);
                if (J::clear_exception(jni)) {
                    adjust_render::drop(jni, item);
                    item = nullptr;
                }
            }
            adjust_render::drop(jni, block);
        } else {
            item = env->CallStaticObjectMethod(cls_item, m_item_by_name, jname);
            if (J::clear_exception(jni)) {
                adjust_render::drop(jni, item);
                item = nullptr;
            }
        }
        adjust_render::drop(jni, jname);
        if (item == nullptr) {
            continue; // legacyItem == null never matches a stack
        }
        entry.item_id = minecraft::item_get_id(jni, item);
        J::clear_exception(jni);
        adjust_render::drop(jni, item);
        g_entries.push_back(entry);
    }
}

} // namespace

bool modern_item_color(events::JniContext* jni, jobject stack, adjust::Argb* out_rgb) noexcept {
    if (stack == nullptr) {
        return false;
    }
    if (!g_resolved) {
        resolve(jni);
    }
    if (g_entries.empty()) {
        return false;
    }
    jobject item = minecraft::item_stack_get_item(jni, stack);
    if (item == nullptr) {
        return false;
    }
    const int id = minecraft::item_get_id(jni, item);
    adjust_render::drop(jni, item);
    const int metadata = adjust_render::stack_metadata(jni, stack);

    bool have_fallback = false;
    adjust::Argb fallback = 0;
    for (const Entry& entry : g_entries) {
        if (entry.item_id != id) {
            continue;
        }
        if (entry.metadata == metadata) {
            *out_rgb = entry.rgb;
            return true;
        }
        if (entry.metadata == -1) {
            fallback = entry.rgb;
            have_fallback = true;
        }
    }
    if (have_fallback) {
        *out_rgb = fallback;
    }
    return have_fallback;
}

} // namespace slinky::modules::adjust_esp
