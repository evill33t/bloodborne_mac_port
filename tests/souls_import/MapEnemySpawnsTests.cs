// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

static class MapEnemySpawnsTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    public static void Main(string[] args) {
        string root=args[0],native=args[1],defs=args[2];var originals=new Dictionary<string,byte[]>();
        byte[]? Read(string path){string p=Path.Combine(root,path);if(!File.Exists(p))return null;var b=File.ReadAllBytes(p);originals.TryAdd(p,SHA256.HashData(b));return b;}
        byte[] baseline=File.ReadAllBytes(Path.Combine(native,"param/gameparam/gameparam.parambnd.dcx")),composed=baseline;
        Check(MapItemPolicy.Map(MapItemPolicy.SourceGoodsCategory,500,3,false)?.TargetID==1500,"Humanity does not become Madman's Knowledge");
        Check(MapItemPolicy.Map(MapItemPolicy.SourceGoodsCategory,501,2,false)?.TargetID==1501,"Twin Humanities does not become Great One's Wisdom");
        Check(MapItemPolicy.Map(0,500,1,false)==null,"Equipment IDs leaked into the goods inventory");
        Check(MapItemPolicy.Map(MapItemPolicy.SourceGoodsCategory,2021,1,true)?.TargetID==902021,"Source keys lost their independent identity");
        var composedFiles=new Dictionary<string,byte[]>();
        var sourceFlags=new Dictionary<int,int>();var flagOwners=new Dictionary<int,int>();
        byte[]? Native(string path)=>composedFiles.GetValueOrDefault(path)??(File.Exists(Path.Combine(native,path))?File.ReadAllBytes(Path.Combine(native,path)):null);
        var allModels=new HashSet<string>();int total=0;
        var donorAnimation=BND4.Read(File.ReadAllBytes(Path.Combine(native,"chr/c2610.anibnd.dcx")));
        static bool Torch(TAE.Event e)=>e.Type==100&&BitConverter.ToInt32(e.GetParameterBytes(false),0)==610250;
        // TAE stores parameter pointers rather than lengths. Removing a neighbour
        // can change the zero alignment padding included by its reader.
        static bool SamePayload(byte[] a,byte[] b) {
            int common=Math.Min(a.Length,b.Length);
            return Math.Abs(a.Length-b.Length)<16&&a.AsSpan(0,common).SequenceEqual(b.AsSpan(0,common))&&
                a.Skip(common).All(v=>v==0)&&b.Skip(common).All(v=>v==0);
        }
        static bool SameEvent(TAE.Event a,TAE.Event b)=>a.Type==b.Type&&a.StartTime==b.StartTime&&a.EndTime==b.EndTime&&a.Unk04==b.Unk04&&a.Group?.GroupType==b.Group?.GroupType&&SamePayload(a.GetParameterBytes(false),b.GetParameterBytes(false));
        int torchActors=0,nonTorchActors=0;
        foreach(var id in new[]{"m10_02_00_00","m10_01_00_00"}) {
            var source=MSB1.Read(Read("map/MapStudio/"+id+".msb")!);
            var map=MSBB.Read(File.ReadAllBytes(Path.Combine(native,"map/MapStudio/m24_01_00_01.msb.dcx")));MapReplacement.ParkActors(map);
            var slots=map.Parts.Collisions.DistinctBy(p=>p.ModelName).ToArray();var chunks=slots.Select(p=>new MapCollisionBindings.Chunk(source.Parts.Collisions.First().Name,[],[])).ToArray();
            var offset=new Vector3(-143.8f,20.07f,164.63f);
            var result=MapEnemySpawns.Bake(Read,source,id,defs,native,map,offset,chunks,slots,composed,readNative:Native);
            string aiPath="script/"+(id=="m10_02_00_00"?"m24_01_00_00":"m24_00_00_00")+".luabnd.dcx";
            var beforeAi=BND4.Read(Native(aiPath)!);var afterAi=BND4.Read(result.Files[aiPath]);var donorAi=File.ReadAllBytes(Path.Combine(native,"script/m22_00_00_00.luabnd.dcx"));
            foreach(var f in beforeAi.Files.Where(f=>Leaf(f.Name).EndsWith(".lua")))Check(afterAi.Files.Single(n=>n.ID==f.ID&&n.Name==f.Name).Bytes.Span.SequenceEqual(f.Bytes.Span),"Native AI export changed an existing destination script");
            var goals=LUAINFO.Read(afterAi.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);
            var beforeGoals=LUAINFO.Read(beforeAi.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);
            Check(beforeGoals.Goals.All(g=>goals.Goals.Any(n=>n.ID==g.ID&&n.Name==g.Name&&n.BattleInterrupt==g.BattleInterrupt&&n.LogicInterrupt==g.LogicInterrupt&&n.LogicInterruptName==g.LogicInterruptName)),"Native AI export changed an existing goal");
            var oldNames=LUAGNL.Read(beforeAi.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl")).Bytes);var newNames=LUAGNL.Read(afterAi.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl")).Bytes);
            Check(newNames.Globals.Take(oldNames.Globals.Count).SequenceEqual(oldNames.Globals),"Native AI export reordered existing global names");
            Check(goals.Goals.Any(g=>g.ID==261010)&&goals.Goals.Any(g=>g.ID==261000),"Imported rig lacks native logic/battle goal registration");
            Check(MapNativeAi.Merge(result.Files[aiPath],donorAi).SequenceEqual(result.Files[aiPath]),"Native AI composition is not idempotent");
            Check(afterAi.Files.Count(f=>Leaf(f.Name).StartsWith("2610")&&Leaf(f.Name).EndsWith(".lua"))==6,"Native rig AI companion scripts were omitted");
            Check(result.Spawns.Count==(id=="m10_02_00_00"?6:86),"Ordinary hollow selection changed or conditional populations were included");
            Check(result.Skipped.Count+result.Spawns.Count==source.Parts.Enemies.Count,"Source actors were not completely accounted for");
            Check(result.Spawns.All(p=>source.Parts.Enemies.Single(s=>s.Name==p.SourceName).InitAnimID==-1),"A hanging/special-pose actor was spawned in a standing pose");
            var old=BND4.Read(composed);composed=result.Files["param/gameparam/gameparam.parambnd.dcx"];var game=BND4.Read(composed);
            foreach(var f in old.Files.Where(f=>Leaf(f.Name)!="NpcParam.param"))Check(game.Files.Single(n=>n.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Enemy export overwrote an unrelated parameter entry");
            var oldNpcs=MapCharacters.NativeRows(old.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
            var npcs=MapCharacters.NativeRows(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
            for(int i=0;i<oldNpcs.Rows.Count;i++)Check(npcs.Rows[i].ID==oldNpcs.Rows[i].ID&&MapCharacters.Payload(npcs.Rows[i]).SequenceEqual(MapCharacters.Payload(oldNpcs.Rows[i])),"Existing composed NPC row changed");
            foreach(var group in result.Spawns.GroupBy(p=>p.TargetModel)) {
                Check(allModels.Add(group.Key),"Two maps reuse an imported character namespace");
                bool hasTorch=group.First().SourceNPCParamID==250001;
                Check(group.All(p=>p.HasSourceTorch==hasTorch),"Source torch loadout was misclassified");
                if(hasTorch)torchActors+=group.Count();else nonTorchActors+=group.Count();
                var animation=BND4.Read(result.Files["chr/"+group.Key+".anibnd.dcx"]);
                foreach(var donor in donorAnimation.Files.Where(f=>TAE.Is(f.Bytes.ToArray()))) {
                    var before=TAE.Read(donor.Bytes);var after=TAE.Read(animation.Files.Single(f=>f.ID==donor.ID).Bytes);
                    Check(after.ID==200000+int.Parse(group.Key[1..])&&before.Animations.Select(a=>a.ID).SequenceEqual(after.Animations.Select(a=>a.ID)),"Torch filtering changed animation identities");
                    Check(before.Animations.Sum(a=>a.Events.Count(Torch))==4,"Native torch fixture changed");
                    foreach(var (a,b) in before.Animations.Zip(after.Animations)) {
                        var expectedEvents=a.Events.Where(e=>hasTorch||!Torch(e)).ToArray();
                        Check(expectedEvents.Length==b.Events.Count,"Torch filter changed event count: "+a.ID);
                        foreach(var (e,n) in expectedEvents.Zip(b.Events))Check(SameEvent(e,n),$"Retained event changed: {a.ID}/{e.Type}, times {e.StartTime}/{n.StartTime} {e.EndTime}/{n.EndTime}, unknown {e.Unk04}/{n.Unk04}, group {e.Group?.GroupType}/{n.Group?.GroupType}, bytes {Convert.ToHexString(e.GetParameterBytes(false))}/{Convert.ToHexString(n.GetParameterBytes(false))}");
                    }
                }
                var bnd=BND4.Read(result.Files["chr/"+group.Key+".chrbnd.dcx"]);var flver=FLVER2.Read(bnd.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
                var bank=TPF.Read(bnd.Files.Single(f=>TPF.Is(f.Bytes.ToArray())).Bytes);
                Check(bank.Textures.All(t=>t.Header.Unk2==0xD),"PS4 texture lacks native tiling metadata");
                Check(flver.Meshes.Count>0&&group.All(p=>p.DirectWeightShare>.6&&p.Bones.Where(b=>new[]{"head","pelvis","lfoot","rfoot","lhand","rhand"}.Contains(new string(b.Source.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray()))).All(b=>b.Direct)),"Hollow skin was substituted or poorly matched: "+group.First().SourceModel+"/"+group.First().SourceNPCParamID+" weight="+group.First().DirectWeightShare+" fallback="+string.Join(",",group.First().Bones.Where(b=>!b.Direct).Select(b=>b.Source+"->"+b.Target)));
                Check(group.All(p=>p.Bones.Where(b=>new[]{"rweapon","lshield","sword","bpw00"}.Contains(new string(b.Source.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray()))).All(b=>b.Direct)),"Source weapon/shield fell back to an arm bone");
                var param=npcs.Rows.Single(r=>r.ID==group.First().NPCParamID);var expected=MapCharacters.Payload(oldNpcs.Rows.Single(r=>r.ID==261010));foreach(int i in new[]{0x146,0x147,0x14A,0x14B})expected[i]=255;
                Check(MapCharacters.Payload(param).SequenceEqual(expected),"Native combat/AI donor changed beyond display masks");
                foreach(var spawn in group) {
                    var actor=map.Parts.Enemies.Single(p=>p.EntityID==spawn.EntityID);var original=source.Parts.Enemies.Single(p=>p.Name==spawn.SourceName);
                    Check(actor.Position==original.Position+offset&&actor.Rotation==original.Rotation&&Vector3.Distance(actor.Scale,original.Scale*spawn.ProportionScale)<.0001f,"Enemy source transform changed");
                    Check(actor.ThinkParamID==261010&&actor.TalkID==-1&&actor.InitAnimID==-1&&actor.CharaInitID==-1,"Enemy retained incompatible source behavior IDs");
                    Check(spawn.DisplayMasks.Length>0,"Source equipment masks were omitted");
                }
            }
            var back=MSBB.Read(map.Write());Check(back.Parts.Enemies.Count==result.Spawns.Count&&back.Parts.Enemies.All(a=>back.Models.Enemies.Any(m=>m.Name==a.ModelName)),"Enemy MSB references failed serialization");
            var loot=MapItems.Bake(Read,Native,source,map,id,defs,native,offset,chunks,slots,result.Spawns,composed);
            foreach(var lot in loot.Lots.Where(l=>l.SourceCollectionFlag>0)) {
                Check(lot.CollectionFlag is >=52100600 and <=52100899,"Pickup used an unallocated native flag group");
                Check(!sourceFlags.TryGetValue(lot.SourceCollectionFlag,out int previousFlag)||previousFlag==lot.CollectionFlag,"A shared source flag changed across maps");
                Check(!flagOwners.TryGetValue(lot.CollectionFlag,out int previousOwner)||previousOwner==lot.SourceCollectionFlag,"Independent source pickups share a collection flag");
                sourceFlags[lot.SourceCollectionFlag]=lot.CollectionFlag;flagOwners[lot.CollectionFlag]=lot.SourceCollectionFlag;
            }
            var next=BND4.Read(loot.GameParamBytes);
            foreach(var f in game.Files.Where(f=>!new[]{"NpcParam.param","ItemLotParam.param","EquipParamGoods.param"}.Contains(Leaf(f.Name))))
                Check(next.Files.Single(n=>n.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Loot conversion changed an unrelated parameter table");
            foreach(var (table,type,size,version) in new[]{("ItemLotParam","ITEMLOT_PARAM_ST",148,(short)2),("EquipParamGoods","EQUIP_PARAM_GOODS_ST",108,(short)1),("NpcParam","NPC_PARAM_ST",388,(short)5)}) {
                var before=ImportParam.Read(game.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes,type,size,version);
                var after=ImportParam.Read(next.Files.Single(f=>Leaf(f.Name)==table+".param").Bytes,type,size,version);
                foreach(var group in before.Rows.Where(r=>table!="NpcParam"||!result.Spawns.Any(s=>s.NPCParamID==r.ID)).GroupBy(r=>r.ID)) {
                    // Native ItemLotParam intentionally includes duplicate IDs.
                    var same=after.Rows.Where(r=>r.ID==group.Key).ToArray();var previousRows=group.ToArray();
                    Check(same.Length==previousRows.Length&&previousRows.Zip(same).All(pair=>pair.First.Name==pair.Second.Name&&ImportParam.Payload(pair.First).SequenceEqual(ImportParam.Payload(pair.Second))),"Loot conversion modified an existing native/composed row");
                }
            }
            var itemLots=ImportParam.Read(next.Files.Single(f=>Leaf(f.Name)=="ItemLotParam.param").Bytes,"ITEMLOT_PARAM_ST",148,2);
            var vial=ImportParam.Payload(itemLots.Rows.Single(r=>r.ID==loot.VialDropLot));
            Check(BitConverter.ToInt32(vial,0)==1000&&BitConverter.ToUInt16(vial,64)==300&&BitConverter.ToUInt16(vial,66)==700&&vial[138]==1&&BitConverter.ToInt32(vial,128)==-1,"Vial probability or repeatability is incorrect");
            var keys=ImportParam.Read(next.Files.Single(f=>Leaf(f.Name)=="EquipParamGoods.param").Bytes,"EQUIP_PARAM_GOODS_ST",108,1);
            foreach(var (sourceID,targetID) in loot.QuestItems)Check(keys.Rows.Single(r=>r.ID==targetID).Name=="Imported DS1R good "+sourceID,"Quest item identity was lost");
            if(id=="m10_02_00_00") {
                Check(loot.Lots.Any(l=>l.SourceID==1020000&&l.Goods.Any(g=>g.TargetID==1500&&g.Count==3)),"Firelink Humanity pickup was omitted");
                Check(loot.Pickups.Any(p=>p.SourcePart=="o0500_0000"),"Firelink Humanity has no native pickup carrier");
            }
            Check(loot.Pickups.Count>0&&loot.Skipped.Count+loot.Pickups.Count==source.Events.Treasures.Count,"Source treasures are not fully accounted for");
            var withItems=MSBB.Read(map.Write());
            foreach(var p in loot.Pickups) {
                var obj=withItems.Parts.Objects.Single(o=>o.Name==p.TargetPart);
                Check(obj.Position==p.Position&&withItems.Parts.Collisions.Any(c=>c.Name==obj.CollisionName),"Pickup transform/collision reference did not round trip");
                Check(withItems.Events.Treasures.Any(t=>t.TreasurePartName==p.TargetPart&&t.ItemLot1==p.Lots[0]),"Pickup treasure references did not round trip");
            }
            foreach(var (path,bytes) in loot.Files)composedFiles[path]=bytes;
            composed=loot.GameParamBytes;
            Console.WriteLine($"PASS: {id}: {loot.Pickups.Count} pickups, {loot.QuestItems.Count} quest identities, {loot.Lots.Count} source loot rows, native vial roll, preserved parameters");
            total+=result.Spawns.Count;
        }
        foreach(var (p,hash) in originals)Check(SHA256.HashData(File.ReadAllBytes(p)).SequenceEqual(hash),"Source character asset was modified");
        Check(torchActors>0&&nonTorchActors>0,"Both torch and non-torch loadouts must be covered");
        Console.WriteLine($"PASS: {torchActors} source torch carriers retain donor flames; {nonTorchActors} other hollows omit only four native torch events, retaining combat/status data and every other animation event");
        Console.WriteLine($"PASS: {total} ordinary hollow placements, source masks/shared textures, independent native rigs, composed opaque parameters, MSB references, PS4 tiling and {originals.Count} unchanged source assets");
    }
}
