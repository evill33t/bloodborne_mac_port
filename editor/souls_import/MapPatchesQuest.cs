// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class MapPatchesQuest {
    internal const string Model="c8911";
    internal const int Entity=4056320,Npc=891100,FirstEvent=12416510;
    internal sealed record Pose(string Archive,int Source,int Frames,float Duration,IReadOnlyList<int> NativeCarriers,bool Approximation);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,string SourceName,int Entity,int Npc,
        IReadOnlyList<NpcArmorPart> Appearance,int FaceGenID,Pose Pose,MapPatchesDialogue.Result Dialogue,int GiftLot,int CollectionFlag,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Patches Catacombs encounter: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static int F(int source)=>MapNpcQuestState.Flag(source);
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,args);
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string nativeRoot,string targetMap,byte[] script,MapItems.Lot reward) {
        Need(targetMap=="m40_05_00_00"&&slots.Count==chunks.Count&&slots.Count>0,"map/collision contract");
        var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==6320);
        Need(from.ModelName=="c0000"&&from.CharaInitID==6320&&from.NPCParamID==6320&&from.ThinkParamID==6320&&from.TalkID==130632&&from.InitAnimID==-1&&from.Scale==Vector3.One,"source NPC identity changed");
        Need(target.Parts.Objects.Any(p=>p.EntityID==MapCatacombsBridges.Lever&&p.ModelName=="o696100")&&target.Parts.Objects.Any(p=>p.EntityID==MapCatacombsBridges.Bridge&&p.ModelName=="o696101"),"the real animated Patches bridge and lever must be installed first");
        Need(!target.Models.Enemies.Any(m=>m.Name==Model)&&target.Parts.GetEntries().All(p=>p.EntityID!=Entity&&p.Name!="souls_patches_catacombs")&&target.Regions.GetEntries().All(p=>p.EntityID!=Entity)&&target.Events.GetEntries().All(p=>p.EntityID!=Entity),"owned NPC identity occupied");
        Need(composed("chr/"+Model+".chrbnd.dcx")==null&&composed("chr/"+Model+".anibnd.dcx")==null,"owned NPC archives occupied");
        var appearance=NpcAppearance.Load(source,definitions,6320,applyBodyShape:true);
        Need(appearance.Sex==1&&appearance.FaceGenID==6320&&appearance.Parts.Single(p=>p.Slot=="Body").RequestedID==301010&&appearance.Parts.Single(p=>p.Slot=="Arms").RequestedID==302010&&appearance.Parts.Single(p=>p.Slot=="Legs").RequestedID==303010&&appearance.Parts.Any(p=>p.Slot=="Face"),"actual Patches face/Black Leather equipment changed");
        var files=new Dictionary<string,byte[]>();var skin=MapCharacters.Package(appearance.Model.Write(),appearance.Textures,"DS1R","c8030",Model,native("chr/c8030.chrbnd.dcx"),native("chr/c8030.anibnd.dcx"),native("action/script/c8030.hks"),files,preserveSourceDimensions:true);
        // Source MSB has no explicit initial pose. 2004[45](6320,8478) is
        // NPC Humanity storage, as the shipped common.emedf states, not8478
        // motion. Use the known unarmed standing pose until armed idle/AI is ported.
        var loaded=MapCharacterAnimations.Load(source,"c0000",0);var animations=BND4.Read(files["chr/"+Model+".anibnd.dcx"]);
        var encoded=NativeAnimation.Convert(loaded.Animation,animations.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),skin.Bones,skin.ProportionScale,preserveSourceWorld:true);
        int[] carriers=[0,1000000,2000000];foreach(int id in carriers)animations.Files.Single(f=>f.ID==1000000000+id).Bytes=encoded;
        var taeFile=animations.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);foreach(var a in tae.Animations){a.Events.Clear();a.EventGroups.Clear();}taeFile.Bytes=tae.Write();files["chr/"+Model+".anibnd.dcx"]=animations.Write(animations.Compression);
        byte[] Read(string path)=>composed(path)??native(path);
        var game=BND4.Read(Read("param/gameparam/gameparam.parambnd.dcx"));var lots=ImportParam.Read(game.Files.Single(f=>Leaf(f.Name)=="ItemLotParam.param").Bytes,"ITEMLOT_PARAM_ST",148,2);
        var giftRow=lots.Rows.SingleOrDefault(r=>r.ID==reward.TargetID)??throw new InvalidDataException("The source Patches gift must be baked before the encounter");var giftData=ImportParam.Payload(giftRow);
        Need(BitConverter.ToInt32(giftData)==MapItemPolicy.MadmansKnowledge&&BitConverter.ToInt32(giftData,32)==MapItemPolicy.NativeGoodsCategory&&giftData[138]==1&&BitConverter.ToInt32(giftData,128)==reward.CollectionFlag,"composed Patches item grant differs from its item policy report");
        var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);var donor=rows.Rows.Single(r=>r.ID==803000);var data=MapCharacters.Payload(donor);
        BitConverter.GetBytes(-1).CopyTo(data,4);data[0x149]&=0xdf;foreach(int index in new[]{0x146,0x147,0x14a,0x14b})data[index]=255;MapCharacters.CloneRow(rows,donor,Npc,"Imported Patches Catacombs6320",data);npcFile.Bytes=MapCharacters.WriteRows(rows);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));
        var dialogue=MapPatchesDialogue.Export(source,native,composed,nativeRoot,definitions,files);
        var events=Events(script,source("event/m13_00_00_00.emevd.dcx")??throw new FileNotFoundException("Patches source event unavailable"),source("event/common.emevd.dcx")??throw new FileNotFoundException("Patches common gift event unavailable"),reward);
        var nativeMap=MSBB.Read(native("map/MapStudio/m21_00_00_00.msb.dcx"));var template=nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030");
        var model=(MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c8030").DeepCopy();model.Name=Model;model.SibPath=model.SibPath.Replace("c8030",Model,StringComparison.OrdinalIgnoreCase);
        var actor=(MSBB.Part.Enemy)template.DeepCopy();actor.Name="souls_patches_catacombs";actor.ModelName=Model;actor.EntityID=Entity;actor.NPCParamID=Npc;actor.ThinkParamID=-1;actor.CharaInitID=-1;actor.TalkID=MapPatchesDialogue.Talk;
        actor.Position=from.Position+offset;actor.Rotation=from.Rotation;actor.Scale=from.Scale*skin.ProportionScale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,actor.Position)].Name;
        actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Clear(actor.DispGroups);
        target.Models.Enemies.Add(model);target.Parts.Enemies.Add(actor);
        return new(files,events,from.Name,Entity,Npc,appearance.Parts,6320,new(loaded.Archive,0,loaded.Animation.FrameCount,loaded.Animation.Duration,carriers,true),dialogue,reward.TargetID,reward.CollectionFlag,[
            "The Catacombs first cleric question, both answers/recovery, late meeting, animated bridge trap, apology/forgiveness choice, one-time source Humanity gift (Madman's Knowledge), repeats and NPC death persist. Later Tomb/Firelink movement, shop and hostile combat are not adapted.",
            "Patches uses actual FaceGen6320 and Black Leather equipment with source body customization. The known source unarmed standing pose substitutes for equipment-driven armed idle; source spear/shield models, armed animation/AI and cloth are not included in this bounded friendly encounter.",
            "Bloodborne has no imported Way of White membership service. The source nonmember refusal response is used; covenant-specific cleric refusal is not claimed. Native pose/talk/voice/item/trap and reload need in-game validation."]);
    }
    internal static byte[] Events(byte[] native,byte[] source,byte[] common,MapItems.Lot reward) {
        Need(Convert.ToHexStringLower(SHA256.HashData(source))=="691516e02235c8770629d032b6512e2ce1e14844487b198a1ee88756207431ba","source Catacombs event resource changed");
        Need(reward.SourceID==1200&&reward.TargetID==90001200&&reward.CollectionFlag>0&&reward.Goods.Count==1&&reward.Goods[0].SourceID==500&&reward.Goods[0].TargetID==MapItemPolicy.MadmansKnowledge&&reward.Goods[0].Count==1,"source Patches gift policy changed");
        var gift=I(2000,0,21,911,11300591,1200,1);Need(EMEVD.Read(common).Events.Single(e=>e.ID==0).Instructions.Any(i=>i.Bank==gift.Bank&&i.ID==gift.ID&&i.ArgData.SequenceEqual(gift.ArgData)),"source common gift request changed");
        var script=EMEVD.Read(native);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+5),"owned Patches event namespace occupied");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var init=new[]{I(2004,1,Entity,(byte)0),I(2004,5,Entity,(byte)1),I(1003,1,(byte)2,(byte)1,(byte)0,F(1620)),I(1003,1,(byte)1,(byte)1,(byte)0,F(1621)),I(2004,5,Entity,(byte)0)};
        foreach(var p in pre.Parameters)p.InstructionIndex+=init.Length;pre.Instructions.InsertRange(0,init);var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Event Event(int n){var e=new EMEVD.Event(FirstEvent+n,EMEVD.Event.RestBehaviorType.Restart);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));return e;}
        // Re-enable after all preconstructor initializers have run. A newly
        // added visibility prefix can execute before the shared fresh-state
        // initializer; a hide-only listener would lose Patches on first load.
        var presence=Event(0);presence.Instructions.AddRange([
            I(3,0,(sbyte)-1,(byte)1,(byte)0,F(1620)),I(3,0,(sbyte)-1,(byte)1,(byte)0,F(1621)),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(2004,5,Entity,(byte)1),
            I(3,0,(sbyte)1,(byte)0,(byte)0,F(1620)),I(3,0,(sbyte)1,(byte)0,(byte)0,F(1621)),I(0,0,(sbyte)0,(byte)1,(sbyte)1),
            I(1003,2,(byte)0,(byte)1,(byte)0,F(1628)),I(2004,5,Entity,(byte)0),I(1000,4,(byte)1)]);
        var grant=Event(1);grant.Instructions.AddRange([I(3,0,(sbyte)0,(byte)1,(byte)0,F(11300591)),I(1003,1,(byte)1,(byte)1,(byte)0,reward.CollectionFlag),I(2003,4,reward.TargetID)]);
        var death=Event(2);death.Instructions.AddRange([I(3,0,(sbyte)-1,(byte)1,(byte)0,F(1620)),I(3,0,(sbyte)-1,(byte)1,(byte)0,F(1621)),I(0,0,(sbyte)1,(byte)1,(sbyte)-1),I(4,0,(sbyte)1,Entity,(byte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)1)]);
        for(int n=1620;n<=1631;n++)death.Instructions.Add(I(2003,2,F(n),(byte)0));death.Instructions.Add(I(2003,2,F(1628),(byte)1));return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
