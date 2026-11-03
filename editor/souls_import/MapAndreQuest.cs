// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

internal static class MapAndreQuest {
    internal const int Entity=4036190,Npc=891500,FirstEvent=12416630;
    internal const string Model="c8915";
    internal sealed record Appearance(int Vertices,int Triangles,double DirectWeightShare,int NativeBones,int SourceBones,int NativeClips,
        string SourceArchive,int SourcePose,int SourceFrames,float SourceDuration,IReadOnlyList<string> SourcePaths,IReadOnlyList<string> TextureFallbacks);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,string SourceName,int Entity,int Npc,
        Appearance Character,MapAndreDialogue.Result Dialogue,MapAndreShop.Result Shop,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Andre encounter: "+why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static int F(int source)=>MapNpcQuestState.Flag(source);
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,args);
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string nativeRoot,string targetMap,byte[] script) {
        Need(targetMap=="m40_03_00_00"&&chunks.Count==slots.Count&&slots.Count>0,"map/collision contract");
        var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==6190);
        Need(from.Name=="c2640_0000"&&from.ModelName=="c2640"&&from.CharaInitID==-1&&from.NPCParamID==264000&&from.ThinkParamID==264000&&from.TalkID==101619&&from.InitAnimID==9000&&from.Scale==Vector3.One,"source forge actor changed");
        Need(!target.Models.Enemies.Any(m=>m.Name==Model)&&target.Parts.GetEntries().All(p=>p.EntityID!=Entity)&&target.Regions.GetEntries().All(p=>p.EntityID!=Entity)&&target.Events.GetEntries().All(p=>p.EntityID!=Entity),"owned actor identity occupied");
        foreach(string path in new[]{"chr/"+Model+".chrbnd.dcx","chr/"+Model+".anibnd.dcx","action/script/"+Model+".hks"})Need(composed(path)==null,"owned character resource occupied: "+path);
        var files=new Dictionary<string,byte[]>();var appearance=MapAndreAppearance.Apply(source,native,definitions,files);
        var game=BND4.Read(composed("param/gameparam/gameparam.parambnd.dcx")??native("param/gameparam/gameparam.parambnd.dcx"));var npcFile=game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");var rows=MapCharacters.NativeRows(npcFile.Bytes,false);
        Need(rows.Rows.All(r=>r.ID!=Npc),"owned NpcParam occupied");var donor=rows.Rows.Single(r=>r.ID==803000);var data=MapCharacters.Payload(donor);BitConverter.GetBytes(-1).CopyTo(data,4);data[0x149]&=0xdf;foreach(int index in new[]{0x146,0x147,0x14a,0x14b})data[index]=255;
        MapCharacters.CloneRow(rows,donor,Npc,"Imported Andre Parish6190",data);npcFile.Bytes=MapCharacters.WriteRows(rows);files.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));
        var shop=MapAndreShop.Apply(source,native,composed,definitions,nativeRoot,files);var dialogue=MapAndreDialogue.Export(source,native,composed,nativeRoot,definitions,files);
        var events=Events(script,source("event/m10_01_00_00.emevd.dcx")??throw new FileNotFoundException("Andre source events unavailable"));
        var nativeMap=MSBB.Read(native("map/MapStudio/m21_00_00_00.msb.dcx"));var template=nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030");var entry=(MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c8030").DeepCopy();entry.Name=Model;entry.SibPath=entry.SibPath.Replace("c8030",Model,StringComparison.OrdinalIgnoreCase);
        var actor=(MSBB.Part.Enemy)template.DeepCopy();actor.Name="souls_andre_parish";actor.ModelName=Model;actor.EntityID=Entity;actor.NPCParamID=Npc;actor.ThinkParamID=-1;actor.CharaInitID=-1;actor.TalkID=dialogue.Talk;
        actor.Position=from.Position+offset;actor.Rotation=from.Rotation;actor.Scale=from.Scale;actor.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,from.CollisionName,actor.Position)].Name;
        actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolInfoName=null!;actor.PatrolType=0;actor.FallbackPlatoonID=0;Array.Clear(actor.MovePointNames);Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);Array.Clear(actor.DispGroups);target.Models.Enemies.Add(entry);target.Parts.Enemies.Add(actor);
        return new(files,events,from.Name,Entity,Npc,new(appearance.Skin.Vertices,appearance.Skin.Triangles,appearance.Skin.DirectWeightShare,appearance.NativeBones,appearance.SourceBones,appearance.NativeClips,
            appearance.SourceArchive,9000,appearance.SourceFrames,appearance.SourceDuration,appearance.SourcePaths,appearance.Skin.TextureFallbacks),dialogue,shop,[
            "Andre is present at his original Parish forge on fresh quest state1320. Source c2640 appearance, head, hammer/tools, all155 visible motion bones and actual hammering9000 use a separate native friendly wrapper; native physical/controller bones retain their retail order. Source appearance dimension and pose are preserved, while native physical/hitboxes remain approximate.",
            "Original introduction/repeat subtitles and English speech precede the native one-item Crest of Artorias purchase UI. Source stock1401 goods2002 retain their single copy,20000 price in Blood Echoes and source stock flag11017140; inventory902002 is shared with the quest-item policy. Source item text is localized; the native key icon is an approximation.",
            "Blacksmith upgrades, repairs, Ember hand-ins, equipment/titanite stock, source hammer impact sound/effects, talk gestures, hostility and full NPC combat are not exposed. Native reaction carriers sample the source hammering with native timing; this is a bounded friendly service. Native purchase/currency/stock, animation, interaction and save/death behavior require gameplay validation."]);
    }
    internal static byte[] Events(byte[] native,byte[] source) {
        Need(Convert.ToHexStringLower(SHA256.HashData(source))=="29472f6360e1c8be472b92dd2c5e553d8cb1dcf3eaf3ef17dc29df5517d55f6f","source Burg event resource changed");
        var sourcePre=EMEVD.Read(source).Events.Single(e=>e.ID==50);
        var expected=I(2000,0,3,11010520,6190,1320,1339,1322);Need(sourcePre.Instructions.Any(i=>i.Bank==expected.Bank&&i.ID==expected.ID&&i.ArgData.SequenceEqual(expected.ArgData)),"source Andre death/quest allocation changed");
        var script=EMEVD.Read(native);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"owned events occupied");var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}
        var init=new[]{I(2004,1,Entity,(byte)0),I(2004,5,Entity,(byte)0),I(1003,1,(byte)1,(byte)0,(byte)0,F(1320)),I(2004,5,Entity,(byte)1)};
        foreach(var p in pre.Parameters)p.InstructionIndex+=init.Length;pre.Instructions.InsertRange(0,init);var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Event Event(int n){var e=new EMEVD.Event(FirstEvent+n,EMEVD.Event.RestBehaviorType.Restart);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));return e;}
        var presence=Event(0);presence.Instructions.AddRange([I(3,0,(sbyte)0,(byte)1,(byte)0,F(1320)),I(2004,5,Entity,(byte)1),I(3,0,(sbyte)0,(byte)0,(byte)0,F(1320)),I(1003,2,(byte)0,(byte)1,(byte)0,F(1322)),I(2004,5,Entity,(byte)0),I(1000,4,(byte)1)]);
        var death=Event(1);death.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,F(1320)),I(4,0,(sbyte)1,Entity,(byte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)1)]);
        for(int f=1320;f<=1339;f++)death.Instructions.Add(I(2003,2,F(f),(byte)0));death.Instructions.Add(I(2003,2,F(1322),(byte)1));return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
