// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class MapOswaldQuest {
    internal const int Entity=4036370,Npc=891400,FirstEvent=12416620;
    internal const string Model="c8914";
    internal sealed record Pose(int Source,int Physical,string Archive,int Frames,float Duration,IReadOnlyList<int> Carriers);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,string SourceName,int Entity,int Npc,
        IReadOnlyList<NpcArmorPart> Appearance,NpcBodyShape.Report BodyShape,Pose Pose,MapOswaldDialogue.Result Dialogue,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Oswald encounter: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static int F(int source)=>MapNpcQuestState.Flag(source);
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,args);
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string nativeRoot,string targetMap,byte[] script) {
        Need(targetMap=="m40_03_00_00"&&chunks.Count==slots.Count&&slots.Count>0,"map/collision contract");
        // Select the real Parish actor, never Firelink's unused DummyEnemy.
        var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==6370);
        Need(from.Name=="c0000_0012"&&from.ModelName=="c0000"&&from.CharaInitID==6370&&from.NPCParamID==6370&&from.ThinkParamID==6370&&from.TalkID==101637&&from.InitAnimID==7865&&from.Scale==Vector3.One,"actual Parish source actor changed");
        Need(!target.Models.Enemies.Any(m=>m.Name==Model)&&target.Parts.GetEntries().All(p=>p.EntityID!=Entity)&&target.Regions.GetEntries().All(p=>p.EntityID!=Entity)&&target.Events.GetEntries().All(p=>p.EntityID!=Entity),"owned actor identity occupied");
        foreach(string path in new[]{"chr/"+Model+".chrbnd.dcx","chr/"+Model+".anibnd.dcx","action/script/"+Model+".hks"})Need(composed(path)==null,"owned character resource occupied: "+path);
        var appearance=NpcAppearance.Load(source,definitions,6370,applyBodyShape:true);
        Need(appearance.Sex==1&&appearance.FaceGenID==6370&&appearance.Parts.Select(p=>p.RequestedID).SequenceEqual(new[]{150002,151002,152002,153002})&&appearance.Parts.All(p=>p.ModelID==9270)&&appearance.BodyShape!=null,"source fully covered Black set changed");
        var files=new Dictionary<string,byte[]>();var skin=MapCharacters.Package(appearance.Model.Write(),appearance.Textures,"DS1R","c8030",Model,native("chr/c8030.chrbnd.dcx"),native("chr/c8030.anibnd.dcx"),native("action/script/c8030.hks"),files,preserveSourceDimensions:true);
        var loaded=new SourcePlayerActions(source).Resolve(7865);Need(loaded.Clip.FrameCount<=256,"source gesture exceeds single-block encoder");var animations=BND4.Read(files["chr/"+Model+".anibnd.dcx"]);
        var encoded=NativeAnimation.Convert(loaded.Clip,animations.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),skin.Bones,skin.ProportionScale,preserveSourceWorld:true);
        int[] carriers=[0,1000000,2000000];foreach(int id in carriers)animations.Files.Single(f=>f.ID==1000000000+id).Bytes=encoded;
        var taeFile=animations.Files.Single(f=>f.ID==3000000);var tae=TAE.Read(taeFile.Bytes);foreach(var a in tae.Animations){a.Events.Clear();a.EventGroups.Clear();}taeFile.Bytes=tae.Write();files["chr/"+Model+".anibnd.dcx"]=animations.Write(animations.Compression);
        var game=BND4.Read(composed("param/gameparam/gameparam.parambnd.dcx")??native("param/gameparam/gameparam.parambnd.dcx"));var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);
        Need(rows.Rows.All(r=>r.ID!=Npc),"owned NpcParam occupied");var donor=rows.Rows.Single(r=>r.ID==803000);var data=MapCharacters.Payload(donor);BitConverter.GetBytes(-1).CopyTo(data,4);data[0x149]&=0xdf;foreach(int index in new[]{0x146,0x147,0x14a,0x14b})data[index]=255;
        MapCharacters.CloneRow(rows,donor,Npc,"Imported Oswald Parish6370",data);npcFile.Bytes=MapCharacters.WriteRows(rows);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));
        var dialogue=MapOswaldDialogue.Export(source,native,composed,nativeRoot,definitions,files);var events=Events(script,source("event/m10_01_00_00.emevd.dcx")??throw new FileNotFoundException("Oswald source events unavailable"));
        var nativeMap=MSBB.Read(native("map/MapStudio/m21_00_00_00.msb.dcx"));var template=nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030");var entry=(MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c8030").DeepCopy();entry.Name=Model;entry.SibPath=entry.SibPath.Replace("c8030",Model,StringComparison.OrdinalIgnoreCase);
        var actor=(MSBB.Part.Enemy)template.DeepCopy();actor.Name="souls_oswald_parish";actor.ModelName=Model;actor.EntityID=Entity;actor.NPCParamID=Npc;actor.ThinkParamID=-1;actor.CharaInitID=-1;actor.TalkID=dialogue.Talk;
        actor.Position=from.Position+offset;actor.Rotation=from.Rotation;actor.Scale=from.Scale*skin.ProportionScale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,actor.Position)].Name;
        actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Clear(actor.DispGroups);target.Models.Enemies.Add(entry);target.Parts.Enemies.Add(actor);
        return new(files,events,from.Name,Entity,Npc,appearance.Parts,appearance.BodyShape!,new(7865,loaded.MotionID,loaded.Archive,loaded.Clip.FrameCount,loaded.Clip.Duration,carriers),dialogue,[
            "Oswald's actual Parish actor appears after ringing the upper Bell of Awakening (source11010700), not merely killing the Gargoyles. His source arrival completion11010581, bounded greetings and death1702 persist. Fresh imported quest state keeps him absent before the bell.",
            "Hunters have no imported Way of White membership. The original nonmember introduction and ordinary non-Hollow greeting are retained as an explicit bounded dialogue policy; source covenant/stat checks are not copied to unrelated BB player fields. Absolution, sin/indictment state, covenant abandonment, shop and player gesture acquisition are not exposed.",
            "The actual fully covered Black armor set, source body customization and arms-out initial pose7865 use private native resources. Source weapons, combat, hostility, cloth and talk gestures are not adapted. Appearance, dialogue, bell timing, death and reload need native gameplay validation."]);
    }
    internal static byte[] Events(byte[] native,byte[] source) {
        Need(Convert.ToHexStringLower(SHA256.HashData(source))=="29472f6360e1c8be472b92dd2c5e553d8cb1dcf3eaf3ef17dc29df5517d55f6f","source Burg events changed");
        var sourceEvent=EMEVD.Read(source).Events.Single(e=>e.ID==11010581);EMEVD.Instruction[] expected=[I(1003,2,(byte)0,(byte)1,(byte)1,0),I(3,0,(sbyte)0,(byte)1,(byte)0,11010700),I(2004,5,0,(byte)1)];
        Need(sourceEvent.Instructions.Count==3&&sourceEvent.Instructions.Zip(expected).All(p=>p.First.Bank==p.Second.Bank&&p.First.ID==p.Second.ID&&p.First.ArgData.SequenceEqual(p.Second.ArgData)),"source upper-bell appearance contract changed");
        var script=EMEVD.Read(native);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"owned native events occupied");var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}
        var init=new[]{I(2004,1,Entity,(byte)0),I(2004,5,Entity,(byte)0),I(1003,1,(byte)3,(byte)1,(byte)0,F(1702)),I(1003,1,(byte)2,(byte)0,(byte)0,F(1700)),I(1003,1,(byte)1,(byte)0,(byte)0,F(11010581)),I(2004,5,Entity,(byte)1)};
        foreach(var p in pre.Parameters)p.InstructionIndex+=init.Length;pre.Instructions.InsertRange(0,init);var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Event Event(int n){var e=new EMEVD.Event(FirstEvent+n,EMEVD.Event.RestBehaviorType.Restart);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));return e;}
        var presence=Event(0);presence.Instructions.AddRange([
            I(3,0,(sbyte)-1,(byte)1,(byte)0,F(11010581)),I(3,0,(sbyte)-1,(byte)1,(byte)0,F(11010700)),I(0,0,(sbyte)1,(byte)1,(sbyte)-1),I(3,0,(sbyte)1,(byte)1,(byte)0,F(1700)),I(0,0,(sbyte)0,(byte)1,(sbyte)1),
            I(2003,2,F(11010581),(byte)1),I(2004,5,Entity,(byte)1),I(3,0,(sbyte)0,(byte)0,(byte)0,F(1700)),I(1003,2,(byte)0,(byte)1,(byte)0,F(1702)),I(2004,5,Entity,(byte)0),I(1000,4,(byte)1)]);
        var death=Event(1);death.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,F(1700)),I(3,0,(sbyte)1,(byte)1,(byte)0,F(11010581)),I(4,0,(sbyte)1,Entity,(byte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)1)]);
        for(int f=1700;f<=1709;f++)death.Instructions.Add(I(2003,2,F(f),(byte)0));death.Instructions.Add(I(2003,2,F(1702),(byte)1));return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
