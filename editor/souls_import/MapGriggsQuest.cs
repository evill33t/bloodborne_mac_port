// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class MapGriggsQuest {
    internal const int FirstEvent=12416600;
    internal sealed record Pose(string Archive,int Source,int Frames,float Duration,bool Approximation);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,string SourceName,string Model,int Entity,int Npc,
        int FaceGenID,IReadOnlyList<NpcArmorPart> Appearance,Pose Pose,MapGriggsDialogue.Result Dialogue,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Griggs encounter: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,args);
    static int F(int source)=>MapNpcQuestState.Flag(source);
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string nativeRoot,string sourceMapName,string targetMap,byte[] script) {
        bool burg=sourceMapName=="m10_01_00_00";
        Need((burg&&targetMap=="m40_03_00_00"||sourceMapName=="m10_02_00_00"&&targetMap=="m40_02_00_00")&&chunks.Count==slots.Count&&slots.Count>0,"source/destination collision contract");
        int sourceEntity=burg?6040:6041,entity=burg?4036040:4026041,npc=burg?891200:891300;string model=burg?"c8912":"c8913";
        var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==sourceEntity);
        Need(from.ModelName=="c0000"&&from.CharaInitID==6040&&from.NPCParamID==6040&&from.ThinkParamID==6040&&from.TalkID==(burg?101604:102604)&&from.InitAnimID==-1&&from.Scale==Vector3.One,"source actor identity changed");
        Need(!target.Models.Enemies.Any(m=>m.Name==model)&&target.Parts.GetEntries().All(p=>p.EntityID!=entity)&&target.Regions.GetEntries().All(p=>p.EntityID!=entity)&&target.Events.GetEntries().All(p=>p.EntityID!=entity),"owned actor identity occupied");
        foreach(string path in new[]{"chr/"+model+".chrbnd.dcx","chr/"+model+".anibnd.dcx","action/script/"+model+".hks"})Need(composed(path)==null,"owned character resource occupied: "+path);
        var appearance=MapGriggsAppearance.Load(source,definitions);var files=new Dictionary<string,byte[]>();
        var skin=MapCharacters.Package(appearance.Model.Write(),appearance.Textures,"DS1R","c8030",model,native("chr/c8030.chrbnd.dcx"),native("chr/c8030.anibnd.dcx"),native("action/script/c8030.hks"),files,preserveSourceDimensions:true);
        var loaded=MapCharacterAnimations.Load(source,"c0000",0);var animations=BND4.Read(files["chr/"+model+".anibnd.dcx"]);
        var encoded=NativeAnimation.Convert(loaded.Animation,animations.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),skin.Bones,skin.ProportionScale,preserveSourceWorld:true);
        foreach(int id in new[]{0,1000000,2000000})animations.Files.Single(f=>f.ID==1000000000+id).Bytes=encoded;
        var taeFile=animations.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);foreach(var a in tae.Animations){a.Events.Clear();a.EventGroups.Clear();}taeFile.Bytes=tae.Write();files["chr/"+model+".anibnd.dcx"]=animations.Write(animations.Compression);
        var game=BND4.Read(composed("param/gameparam/gameparam.parambnd.dcx")??native("param/gameparam/gameparam.parambnd.dcx"));
        var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);Need(rows.Rows.All(r=>r.ID!=npc),"owned NpcParam occupied");
        var donor=rows.Rows.Single(r=>r.ID==803000);var data=MapCharacters.Payload(donor);BitConverter.GetBytes(-1).CopyTo(data,4);data[0x149]&=0xdf;
        foreach(int index in new[]{0x146,0x147,0x14a,0x14b})data[index]=255;MapCharacters.CloneRow(rows,donor,npc,"Imported Griggs "+sourceMapName,data);npcFile.Bytes=MapCharacters.WriteRows(rows);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));
        var dialogue=MapGriggsDialogue.Export(source,native,composed,nativeRoot,definitions,files,burg);
        var events=Events(script,source("event/"+sourceMapName+".emevd.dcx")??throw new FileNotFoundException("Griggs source events unavailable"),burg,entity);
        var nativeMap=MSBB.Read(native("map/MapStudio/m21_00_00_00.msb.dcx"));var template=nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030");
        var entry=(MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c8030").DeepCopy();entry.Name=model;entry.SibPath=entry.SibPath.Replace("c8030",model,StringComparison.OrdinalIgnoreCase);
        var actor=(MSBB.Part.Enemy)template.DeepCopy();actor.Name=burg?"souls_griggs_burg":"souls_griggs_firelink";actor.ModelName=model;actor.EntityID=entity;actor.NPCParamID=npc;actor.ThinkParamID=-1;actor.CharaInitID=-1;actor.TalkID=dialogue.Talk;
        actor.Position=from.Position+offset;actor.Rotation=from.Rotation;actor.Scale=from.Scale*skin.ProportionScale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,actor.Position)].Name;
        actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Clear(actor.DispGroups);
        target.Models.Enemies.Add(entry);target.Parts.Enemies.Add(actor);
        return new(files,events,from.Name,model,entity,npc,6040,appearance.Armor.Parts,new(loaded.Archive,0,loaded.Animation.FrameCount,loaded.Animation.Duration,true),dialogue,[
            "Griggs starts imprisoned in Lower Burg. Only the real Residence Key door action enables source1110→1111; entering imported Firelink enables1111→1112. The Burg actor then disappears while Firelink becomes available. Source rescue/arrival flags and death persist; an untouched save does not spawn late Firelink Griggs.",
            "The original call for help, rescue thanks and resting repeat use source subtitles and English voice. Firelink uses the original neutral arrival greeting and repeat. These are a bounded dialogue adaptation, not a Hunter Intelligence/Bloodtinge eligibility decision: source stat-gated teaching choices, sorcery shop, Logan/Sen's progression, hostility and full quest are not adapted.",
            "Actual FaceGen6040, open Black Sorcerer hat, armor and source body customization are retained. A source unarmed standing idle substitutes for unresolved staff/shield-driven idle. Source equipment attachments, casting, combat and cloth are not adapted. Native animation, dialogue, key action, travel and save behavior require gameplay validation."]);
    }
    internal static byte[] Events(byte[] native,byte[] source,bool burg,int entity) {
        Need(Convert.ToHexStringLower(SHA256.HashData(source))==(burg?"29472f6360e1c8be472b92dd2c5e553d8cb1dcf3eaf3ef17dc29df5517d55f6f":"b519c70b1d3bdbaf7deb0472b47355b827cbae5f61b91c9edd915e41cec0d346"),"source event resource changed");
        var sourceScript=EMEVD.Read(source);
        // Exact source progression contracts, including the map-entry gate.
        var sourceEvent=sourceScript.Events.Single(e=>e.ID==(burg?11010532:11020552));
        EMEVD.Instruction[] expected=burg?[
            I(3,0,(sbyte)1,(byte)0,(byte)0,1114),I(3,0,(sbyte)1,(byte)1,(byte)0,1110),I(3,0,(sbyte)1,(byte)1,(byte)0,11010181),I(0,0,(sbyte)0,(byte)1,(sbyte)1),
            I(2003,22,0,0,(byte)0),I(2003,2,0,(byte)1),I(2004,9,0,-1,-1,-1,-1,-1)]:[
            I(3,0,(sbyte)1,(byte)0,(byte)0,1114),I(3,0,(sbyte)1,(byte)0,(byte)0,1117),I(3,0,(sbyte)1,(byte)1,(byte)0,1111),I(3,8,(sbyte)1,(byte)1,(byte)10,(byte)2),I(0,0,(sbyte)0,(byte)1,(sbyte)1),
            I(2003,22,0,0,(byte)0),I(2003,2,0,(byte)1),I(2004,5,0,(byte)1)];
        Need(sourceEvent.Instructions.Count==expected.Length&&sourceEvent.Instructions.Zip(expected).All(p=>p.First.Bank==p.Second.Bank&&p.First.ID==p.Second.ID&&p.First.ArgData.SequenceEqual(p.Second.ArgData)),"source progression instructions changed");
        var script=EMEVD.Read(native);int first=FirstEvent+(burg?0:10);
        Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=first&&e.ID<first+10),"owned native events occupied");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}int[] visible=burg?[1110,1111]:[1112,1113];
        var init=new[]{I(2004,1,entity,(byte)0),I(2004,5,entity,(byte)1),I(1003,1,(byte)2,(byte)1,(byte)0,F(visible[0])),I(1003,1,(byte)1,(byte)1,(byte)0,F(visible[1])),I(2004,5,entity,(byte)0)};
        foreach(var p in pre.Parameters)p.InstructionIndex+=init.Length;pre.Instructions.InsertRange(0,init);var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Event Event(int n){var e=new EMEVD.Event(first+n,EMEVD.Event.RestBehaviorType.Restart);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));return e;}
        void Stage(EMEVD.Event e,int stage){for(int f=1110;f<=1119;f++)e.Instructions.Add(I(2003,2,F(f),(byte)0));e.Instructions.Add(I(2003,2,F(stage),(byte)1));}
        var progression=Event(0);progression.Instructions.Add(I(3,0,(sbyte)1,(byte)0,(byte)0,F(1114)));
        if(burg)progression.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,F(1110)),I(3,0,(sbyte)1,(byte)1,(byte)0,F(11010181))]);
        else progression.Instructions.AddRange([I(3,0,(sbyte)1,(byte)0,(byte)0,F(1117)),I(3,0,(sbyte)1,(byte)1,(byte)0,F(1111)),I(3,8,(sbyte)1,(byte)1,(byte)40,(byte)2)]);
        progression.Instructions.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)1));Stage(progression,burg?1111:1112);if(!burg)progression.Instructions.Add(I(2004,5,entity,(byte)1));
        var presence=Event(1);foreach(int flag in visible)presence.Instructions.Add(I(3,0,(sbyte)-1,(byte)1,(byte)0,F(flag)));
        presence.Instructions.AddRange([I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(2004,5,entity,(byte)1)]);
        foreach(int flag in visible)presence.Instructions.Add(I(3,0,(sbyte)1,(byte)0,(byte)0,F(flag)));
        presence.Instructions.AddRange([I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(1003,2,(byte)0,(byte)1,(byte)0,F(1115)),I(2004,5,entity,(byte)0),I(1000,4,(byte)1)]);
        var death=Event(2);foreach(int flag in visible)death.Instructions.Add(I(3,0,(sbyte)-1,(byte)1,(byte)0,F(flag)));
        death.Instructions.AddRange([I(0,0,(sbyte)1,(byte)1,(sbyte)-1),I(4,0,(sbyte)1,entity,(byte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)1)]);Stage(death,1115);
        return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
