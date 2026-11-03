// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapHavel {
    internal const int Entity=4039840,Leash=4032799,DeathEvent=12415920;
    internal sealed record Report(string SourceName,int SourceEntity,int Entity,int CharaInit,int Think,string Collision,int SourceMoves,int NativeCopies,int Selectors,IReadOnlyList<string> Limits);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,byte[] Script,Report Details);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Havel map import: "+why);}
    static EMEVD.Instruction Ins(int bank,int id,params object[] args)=>MapBossEncounters.Ins(bank,id,args);
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]?> staged,Func<string,byte[]> native,
        MSB1 sourceMap,MSBB target,Vector3 offset,IReadOnlyList<MapCollisionBindings.Chunk> chunks,IReadOnlyList<MSBB.Part.Collision> slots,
        string definitions,string targetMap,byte[] scriptBytes) {
        var actor=sourceMap.Parts.Enemies.Single(p=>p.EntityID==6580);Need(actor.ModelName=="c0000"&&actor.NPCParamID==6580&&actor.ThinkParamID==6580&&actor.CharaInitID==6580&&actor.InitAnimID==-1,"source Havel placement changed");
        Need(!target.Parts.GetEntries().Any(p=>p.EntityID==Entity)&&!target.Regions.GetEntries().Any(p=>p.EntityID==Leash),"owned actor/region occupied");
        var donor=MSBB.Read(native("map/MapStudio/m24_00_00_00.msb.dcx"));var template=donor.Parts.Enemies.Single(p=>p.Name=="c0000_0010"&&p.CharaInitID==6151);
        if(!target.Models.Enemies.Any(m=>m.Name=="c0000"))target.Models.Enemies.Add((MSBB.Model.Enemy)donor.Models.Enemies.Single(m=>m.Name=="c0000").DeepCopy());
        var imported=(MSBB.Part.Enemy)template.DeepCopy();imported.Name="c0000_ds1_havel";imported.EntityID=Entity;imported.NPCParamID=HavelParameters.Character;imported.ThinkParamID=HavelParameters.Think;imported.CharaInitID=HavelParameters.Character;imported.TalkID=-1;imported.InitAnimID=-1;
        imported.Position=actor.Position+offset;imported.Rotation=actor.Rotation;imported.Scale=actor.Scale;
        imported.CollisionName=slots[MapCollisionBindings.ObjectSlot(chunks,actor.CollisionName,imported.Position)].Name;
        Array.Fill(imported.DrawGroups,uint.MaxValue);Array.Fill(imported.BackreadGroups,uint.MaxValue);Array.Fill(imported.DispGroups,0u);target.Parts.Enemies.Add(imported);
        var leash=sourceMap.Regions.Regions.Single(r=>r.EntityID==1012799);
        target.Regions.Regions.Add(new(){Name="souls_havel_home",EntityID=Leash,Position=leash.Position+offset,Rotation=leash.Rotation,Shape=leash.Shape.DeepCopy()});
        var equipment=HavelEquipment.Build(source,native,definitions);var animation=HavelPlayerAnimations.Build(source,native,equipment.Skin.Bones);var files=equipment.Files.ToDictionary(p=>p.Key,p=>p.Value);
        files.Add("chr/c0000.anibnd.dcx",animation.Main);foreach(var p in animation.NativeSplits)files.Add(p.Key,p.Value);
        string game="param/gameparam/gameparam.parambnd.dcx";files.Add(game,HavelParameters.Build(staged(game)??native(game),source,definitions));
        string ai="script/"+targetMap+".luabnd.dcx";var aiBytes=HavelAi.Build(staged(ai)??native(ai),source,native,Leash);files.Add(ai,aiBytes);
        foreach(var p in MapNativeAi.Fallbacks(aiBytes,staged))files.Add(p.Key,p.Value);
        var script=EMEVD.Read(scriptBytes);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID==DeathEvent),"native event reservation occupied");
        int flag=MapNpcQuestState.Flag(11010860);
        var death=new EMEVD.Event(DeathEvent,EMEVD.Event.RestBehaviorType.Restart);
        death.Instructions.AddRange([
            Ins(1003,1,(byte)3,(byte)1,(byte)0,flag), // Reload after death: hide actor.
            Ins(4,0,(sbyte)0,Entity,(byte)1),
            Ins(2003,2,flag,(byte)1),
            Ins(1000,4,(byte)0),
            Ins(2004,5,Entity,(byte)0),Ins(1000,4,(byte)0)]);
        script.Events.Add(death);var constructor=script.Events.Single(e=>e.ID==0);
        var start=Ins(2000,0,0,(uint)DeathEvent,0u);foreach(var p in constructor.Parameters)p.InstructionIndex++;constructor.Instructions.Insert(0,start);
        return new(files,script.Write(DCX.Type.DCX_DFLT_10000_44_9),new(actor.Name,6580,Entity,HavelParameters.Character,HavelParameters.Think,imported.CollisionName,animation.Bindings.Count,animation.NativeCopies,animation.Selectors,[
            "Havel uses source armor, Dragon Tooth/Greatshield, source core attack/guard/gait poses and a private player-NPC animation bank. Other player actions retain explicit native motion fallbacks; charge preparation, grip transitions and shield bash remain under validation.",
            "Native player physics, damage scaling, stagger, support-bone modifiers and sounds remain approximations. Persistent death is separate from tower doors; key-operated doors and the excluded equipment reward need their own adapters."]));
    }
}
