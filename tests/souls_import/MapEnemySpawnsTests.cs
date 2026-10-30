// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;

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
            Check(result.Spawns.Count==(id=="m10_02_00_00"?19:86),"Enemy selection changed or conditional populations were included");
            Check(result.Skipped.Count+result.Spawns.Count==source.Parts.Enemies.Count,"Source actors were not completely accounted for");
            Check(result.Spawns.All(p=>p.SourceInitAnimID==source.Parts.Enemies.Single(s=>s.Name==p.SourceName).InitAnimID&&
                (p.SourceInitAnimID==-1||id=="m10_02_00_00"&&p.SourceInitAnimID==9001&&p.SourceModel is "c2900" or "c2910")),"An unadapted hanging/special-pose actor was spawned standing");
            var skeletons=result.Spawns.Where(p=>p.SourceModel is "c2900" or "c2910").ToArray();
            Check(skeletons.Length==(id=="m10_02_00_00"?13:0),"Cemetery skeleton population changed");
            if(skeletons.Length>0) {
                Check(skeletons.Count(p=>p.SourceModel=="c2910")==2&&skeletons.All(p=>p.TargetModel.StartsWith("c97")&&p.EntityID is >=2419900 and <2420000&&!p.HasSourceTorch&&p.DirectWeightShare>.99),"Skeleton rig, population or reserved identity changed");
                Check(skeletons.Where(p=>p.SourceModel=="c2910").All(p=>p.ProportionScale>1.5f&&p.ProportionScale<2.5f),"Giant skeleton stature was lost");
                Check(skeletons.Where(p=>p.SourceNPCParamID==290000).All(p=>p.DisplayMasks.SequenceEqual(new[]{0,1}))&&skeletons.Where(p=>p.SourceNPCParamID==290002).All(p=>p.DisplayMasks.SequenceEqual(new[]{3})),"Skeleton sword/shield or greatsword loadout was lost");
                var originalScript=File.ReadAllBytes(Path.Combine(native,"event/m24_01_00_00.emevd.dcx"));
                var wake=MapSkeletonAnimations.Events(Read,originalScript,result.Spawns);var beforeScript=EMEVD.Read(originalScript);var afterScript=EMEVD.Read(wake.Script);
                Check(wake.Wakes.Count==13&&afterScript.Events.Count==beforeScript.Events.Count+13,"Cemetery awakening event population changed");
                foreach(var e in beforeScript.Events) {
                    var following=afterScript.Events.Single(n=>n.ID==e.ID);
                    Check(e.RestBehavior==following.RestBehavior&&e.Instructions.Count+(e.ID==0?13:0)==following.Instructions.Count&&e.Instructions.Zip(following.Instructions).All(p=>p.First.Bank==p.Second.Bank&&p.First.ID==p.Second.ID&&p.First.ArgData.SequenceEqual(p.Second.ArgData)),"Cemetery export changed an existing native event instruction");
                }
                Check(wake.Wakes.All(w=>skeletons.Any(s=>s.EntityID==w.Entity&&s.SourceEntityID==w.SourceEntity)&&skeletons.Any(s=>s.EntityID==w.Trigger&&s.SourceEntityID==w.SourceTrigger)),"Awakening targets reference an unimported actor");
                Check(wake.Wakes.Select(w=>w.Radius).Distinct().Order().SequenceEqual(new[]{3f,4f,7f,11f})&&wake.Wakes.Any(w=>Math.Abs(w.Delay-2.2f)<.0001f),"Source awakening radii or group delays were lost");
                foreach(var w in wake.Wakes) {
                    var instructions=afterScript.Events.Single(e=>e.ID==w.EventID).Instructions;
                    var rise=instructions.Single(i=>i.Bank==2003&&i.ID==18&&BitConverter.ToInt32(i.ArgData,4)==7005);
                    Check(rise.ArgData[8]==0&&rise.ArgData[9]==0,"Awakening waits on an AI-dependent special-animation completion signal");
                    var brains=instructions.Where(i=>i.Bank==2004&&i.ID==19).Select(i=>BitConverter.ToInt32(i.ArgData,4)).ToArray();
                    Check(brains.SequenceEqual(new[]{261091,261010,261010}),"Awakening lost native dormant/combat AI transitions");
                    Check(!instructions.Any(i=>i.Bank==2004&&i.ID is 1 or 43)&&!instructions.Any(i=>i.Bank==2003&&i.ID==18&&BitConverter.ToInt32(i.ArgData,4)==-1),"Awakening disables its native brain or forcibly cancels a later hit reaction");
                }
            }
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
                bool skeletonModel=group.First().SourceModel is "c2900" or "c2910";
                var wakeVariants=(from variant in Enumerable.Range(1,3) from slot in new[]{7004,7005} select (Variant:variant,Slot:slot,ID:(long)variant*1000000+slot)).ToArray();
                Check(animation.Files.Count==donorAnimation.Files.Count+(skeletonModel?6:0)&&animation.Files.Select(f=>f.ID).Distinct().Count()==animation.Files.Count,"Skeleton wake clips changed the native binder outside six declared variants");
                foreach(var original in donorAnimation.Files.Where(f=>!TAE.Is(f.Bytes.ToArray())&&!(f.ID>=7000000&&f.ID<8000000)&&
                    !(skeletonModel&&f.ID>=1000000000&&new[]{2014,2020,2040,7004,7005}.Contains((f.ID-1000000000)%1000000)))) {
                    var retained=animation.Files.Single(f=>f.ID==original.ID);
                    Check(retained.Flags==original.Flags&&retained.Name==original.Name.Replace("c2610",group.Key,StringComparison.OrdinalIgnoreCase)&&retained.Bytes.Span.SequenceEqual(original.Bytes.Span),"Skeleton wake variants changed an unrelated native animation/rig entry");
                }
                foreach(int metadata in new[]{7000300,7000400,7000500,7000501}) {
                    byte[] expectedMetadata=SkinRetarget.RenameBehavior(donorAnimation.Files.Single(f=>f.ID==metadata).Bytes,"c2610",group.Key);
                    if(skeletonModel&&metadata==7000300)foreach(var variant in wakeVariants) {
                        string clip=$"a{variant.Variant:D3}_{variant.Slot:D6}.hkx";
                        byte[] shared=Encoding.ASCII.GetBytes(@"..\..\..\..\Model\chr\c9999\hkx\"+clip+"\0");
                        int at=expectedMetadata.AsSpan().IndexOf(shared);
                        Check(at>=0&&expectedMetadata.AsSpan(at+shared.Length).IndexOf(shared)<0,"Native shared wake reference fixture changed");
                        Encoding.ASCII.GetBytes(@"..\..\..\..\Model\chr\"+group.Key+@"\hkx\"+clip+"\0").CopyTo(expectedMetadata,at);
                    }
                    Check(animation.Files.Single(f=>f.ID==metadata).Bytes.Span.SequenceEqual(expectedMetadata),"Wake path redirection changed unrelated native project/character/behavior data");
                }
                foreach(var donor in donorAnimation.Files.Where(f=>TAE.Is(f.Bytes.ToArray()))) {
                    var before=TAE.Read(donor.Bytes);var after=TAE.Read(animation.Files.Single(f=>f.ID==donor.ID).Bytes);
                    var expectedIDs=before.Animations.Select(a=>a.ID).Concat(skeletonModel?wakeVariants.Select(v=>v.ID):Enumerable.Empty<long>()).Order();
                    Check(after.ID==200000+int.Parse(group.Key[1..])&&expectedIDs.SequenceEqual(after.Animations.Select(a=>a.ID)),"Skeleton/torch filtering changed animation identities outside the declared wake variants");
                    if(skeletonModel)foreach(var variant in wakeVariants) {
                        var original=after.Animations.Single(a=>a.ID==variant.Slot);var copy=after.Animations.Single(a=>a.ID==variant.ID);
                        string name=$"a{variant.Variant:D3}_{variant.Slot:D6}";
                        Check(copy.AnimFileName==name+".hkt"&&copy.MiniHeader is TAE.Animation.AnimMiniHeader.Standard h&&!h.IsLoopByDefault&&!h.ImportsHKX&&!h.AllowDelayLoad&&h.ImportHKXSourceAnimID==-1,"Wake variant lost its own native animation identity");
                        Check(copy.Events.Count==original.Events.Count&&copy.Events.Zip(original.Events).All(p=>SameEvent(p.First,p.Second)),"Wake variant lost source timing/protection gates");
                        Check(copy.EventGroups.Count==original.EventGroups.Count&&copy.Events.Select(e=>e.Group is null?-1:copy.EventGroups.IndexOf(e.Group)).SequenceEqual(original.Events.Select(e=>e.Group is null?-1:original.EventGroups.IndexOf(e.Group))),"Wake variant lost event group membership");
                        var clip=animation.Files.Single(f=>f.ID==1000000000+variant.ID);var baseClip=animation.Files.Single(f=>f.ID==1000000000+variant.Slot);
                        Check(Leaf(clip.Name)==name+".hkx"&&clip.Flags==baseClip.Flags&&clip.CompressionType==baseClip.CompressionType&&clip.Bytes.Span.SequenceEqual(baseClip.Bytes.Span),"Wake variant clip filename/ID/payload does not match its native lookup");
                    }
                    Check(before.Animations.Sum(a=>a.Events.Count(Torch))==4,"Native torch fixture changed");
                    foreach(var a in before.Animations) {
                        var b=after.Animations.Single(n=>n.ID==a.ID);
                        if(group.First().SourceModel is "c2900" or "c2910"&&((a.ID%1000000) is 2014 or 2020 or 2040||a.ID is 7004 or 7005)) {
                            var hkx=animation.Files.Single(f=>f.ID==1000000000+a.ID);var pack=new NativeAnimation.Pack(hkx.Bytes.ToArray());
                            int at=pack.Object("hkaSplineCompressedAnimation");float duration=pack.Float(at+16);
                            Check(duration>0&&b.Events.All(e=>e.StartTime>=0&&e.EndTime<=duration+.0001f),"Converted clip has native gates beyond its source duration");
                            foreach(var e in b.Events.Where(e=>e.Type==0&&new[]{23,78,79,86}.Contains(BitConverter.ToInt32(e.GetParameterBytes(false),0))))Check(e.StartTime>=duration-.101f,"Native AI can cancel source reassembly early");
                            TAE.Event Gate(int jump)=>b.Events.Single(e=>e.Type==0&&BitConverter.ToInt32(e.GetParameterBytes(false),0)==jump);
                            if(a.ID==7004)foreach(int jump in new[]{8,39})Check(Gate(jump).StartTime==0&&Math.Abs(Gate(jump).EndTime-duration)<.0001f,"Dormant pile lost authored damage/collision protection");
                            if(a.ID==7005) {
                                float invincible=group.First().SourceModel=="c2900"?2.5f:1.6666666f,lockOn=group.First().SourceModel=="c2900"?2.5f:2.6666667f;
                                Check(Gate(8).StartTime==0&&Math.Abs(Gate(8).EndTime-invincible)<.0001f&&Gate(49).StartTime==0&&Math.Abs(Gate(49).EndTime-lockOn)<.0001f,"Awakening lost source vulnerability/lock-on timing");
                                Check(!b.Events.Any(e=>e.Type==0&&BitConverter.ToInt32(e.GetParameterBytes(false),0)==39),"Awakening gained an unauthored hurtbox-disable gate");
                                Check(Math.Abs(Gate(24).StartTime-invincible)<.0001f&&Math.Abs(Gate(24).EndTime-(group.First().SourceModel=="c2900"?2.9f:2.6666667f))<.0001f,"Awakening lost its source superarmor interval");
                            }
                            if(a.ID%1000000 is 2014 or 2020 or 2040)Check(Gate(24).StartTime==0&&Gate(24).EndTime>1.9f,"Source recovery lost its superarmor gate");
                            continue;
                        }
                        bool skeletonAttack=group.First().SourceModel is "c2900" or "c2910"&&a.ID is 1003013 or 1003014 or 1003015 or 1003016;
                        bool Armor(TAE.Event e)=>e.Type==0&&BitConverter.ToInt32(e.GetParameterBytes(false),0)==24;
                        if(skeletonAttack)Check(a.Events.Count(Armor)==(a.ID==1003013?1:a.ID==1003015?3:2)&&!b.Events.Any(Armor),"Skeleton basic attack retained donor superarmor");
                        var expectedEvents=a.Events.Where(e=>(hasTorch||!Torch(e))&&(!skeletonAttack||!Armor(e))).ToArray();
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
        Console.WriteLine($"PASS: {torchActors} source torch carriers retain donor flames; {nonTorchActors} other enemies omit only four native torch events, retaining combat/status data; skeleton basic attacks also omit eight donor superarmor windows");
        Console.WriteLine($"PASS: {total} enemy placements (92 hollows, 13 cemetery skeletons), source masks/shared textures, independent native rigs, composed opaque parameters, MSB references, PS4 tiling and {originals.Count} unchanged source assets");
    }
}
