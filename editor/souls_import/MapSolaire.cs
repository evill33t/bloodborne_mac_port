// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// First Burg encounter only. Source summons and later appearances are separate
// actors; accepting the gift does not remove Solaire from the overlook.
internal static class MapSolaire {
    internal const int PresenceEvent=14035960,GiftEvent=14035961;
    internal sealed record Result(byte[] Script,int Actor,int GiftLot,int CollectionFlag,int[] VisibleStates,IReadOnlyList<string> Limitations);
    static EMEVD.Instruction Ins(int bank,int id,params object[] args)=>new(bank,id,args);
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.SequenceEqual(b.ArgData);
    internal static Result Apply(byte[] native,byte[] source,byte[] commonBytes,int actor,MapItems.Lot reward) {
        int F(int flag)=>MapNpcQuestState.Flag(flag);
        if(actor<=0||reward.SourceID!=1000||reward.SourceCollectionFlag!=50000000||reward.CollectionFlag<=0||
           reward.Goods.Count!=1||reward.Goods[0].SourceID!=100||reward.Goods[0].Count!=1||reward.Goods[0].Kind!="quest-item")throw new InvalidDataException("Source Solaire gift identity changed");
        var sourceScript=EMEVD.Read(source);var preSource=sourceScript.Events.Single(e=>e.ID==50);
        EMEVD.Instruction[] visibility=[Ins(1003,1,(byte)4,(byte)1,(byte)0,1007),Ins(1003,1,(byte)3,(byte)1,(byte)0,1004),Ins(1003,1,(byte)2,(byte)1,(byte)0,1001),Ins(1003,1,(byte)1,(byte)1,(byte)0,1000),Ins(2004,5,6001,(byte)0)];
        if(!Enumerable.Range(0,Math.Max(0,preSource.Instructions.Count-visibility.Length+1)).Any(at=>preSource.Instructions.Skip(at).Take(visibility.Length).Zip(visibility).All(p=>Same(p.First,p.Second))))throw new InvalidDataException("Source Solaire visibility selection changed");
        var common=EMEVD.Read(commonBytes);var giftCall=Ins(2000,0,0,911,11010591,1000,1);
        if(!common.Events.Single(e=>e.ID==0).Instructions.Any(i=>Same(i,giftCall)))throw new InvalidDataException("Source Solaire gift initializer changed");
        var progress=sourceScript.Events.Single(e=>e.ID==11010530);
        EMEVD.Instruction[] progression=[Ins(3,0,(sbyte)1,(byte)0,(byte)0,1004),Ins(3,0,(sbyte)1,(byte)1,(byte)0,1000),Ins(3,0,(sbyte)1,(byte)1,(byte)0,11010591),Ins(0,0,(sbyte)0,(byte)1,(sbyte)1),Ins(2003,22,0,0,(byte)0),Ins(2003,2,0,(byte)1),Ins(2004,5,0,(byte)1)];
        if(progress.Instructions.Count!=progression.Length||!progress.Instructions.Zip(progression).All(p=>Same(p.First,p.Second)))throw new InvalidDataException("Source Solaire gift progression changed");
        var script=EMEVD.Read(native);if(script.Format!=EMEVD.Game.Bloodborne||script.Events.Any(e=>e.ID is PresenceEvent or GiftEvent))throw new InvalidDataException("Native Solaire event namespace is occupied or incompatible");
        var pre=script.Events.Single(e=>e.ID==50);var ctor=script.Events.Single(e=>e.ID==0);
        int[] visible=[1000,1001,1004,1007];
        var initial=new List<EMEVD.Instruction>{Ins(2004,5,actor,(byte)1)};
        for(int i=0;i<visible.Length;i++)initial.Add(Ins(1003,1,checked((byte)(visible.Length-i)),(byte)1,(byte)0,F(visible[i])));
        initial.Add(Ins(2004,5,actor,(byte)0));
        foreach(var p in pre.Parameters)p.InstructionIndex+=initial.Count;pre.Instructions.InsertRange(0,initial);
        var presence=new EMEVD.Event(PresenceEvent,EMEVD.Event.RestBehaviorType.Restart);var code=presence.Instructions;
        foreach(int f in visible)code.Add(Ins(3,0,(sbyte)-1,(byte)1,(byte)0,F(f)));
        code.Add(Ins(1000,1,(byte)7,(byte)1,(sbyte)-1));
        code.Add(Ins(2004,5,actor,(byte)0));
        foreach(int f in visible)code.Add(Ins(3,0,(sbyte)-2,(byte)1,(byte)0,F(f)));
        code.Add(Ins(0,0,(sbyte)0,(byte)1,(sbyte)-2));code.Add(Ins(1000,4,(byte)1));
        code.Add(Ins(2004,5,actor,(byte)1));
        foreach(int f in visible)code.Add(Ins(3,0,(sbyte)1,(byte)0,(byte)0,F(f)));
        code.Add(Ins(0,0,(sbyte)0,(byte)1,(sbyte)1));code.Add(Ins(1000,4,(byte)1));
        var gift=new EMEVD.Event(GiftEvent);code=gift.Instructions;
        code.Add(Ins(3,0,(sbyte)0,(byte)1,(byte)0,F(11010591)));
        code.Add(Ins(1003,1,(byte)1,(byte)1,(byte)0,reward.CollectionFlag));code.Add(Ins(2003,4,reward.TargetID));
        code.Add(Ins(1003,1,(byte)32,(byte)0,(byte)0,F(1000)));code.Add(Ins(1003,1,(byte)31,(byte)1,(byte)0,F(1004)));
        // Source1000..1029 spans old and expanded native reservations. Clear
        // only those explicit bits instead of a native contiguous flag range.
        for(int f=1000;f<=1029;f++)code.Add(Ins(2003,2,F(f),(byte)0));
        code.Add(Ins(2003,2,F(1001),(byte)1));code.Add(Ins(2003,2,F(11010591),(byte)0));
        script.Events.Add(presence);script.Events.Add(gift);
        ctor.Instructions.Add(Ins(2000,0,0,PresenceEvent,0));ctor.Instructions.Add(Ins(2000,0,0,GiftEvent,0));
        return new(script.Write(DCX.Type.DCX_DFLT_10000_44_9),actor,reward.TargetID,reward.CollectionFlag,visible.Select(F).ToArray(),[
            "Source Burg appearance states, first two choices, gift and1000-to1001 progression are adapted. The gift remains an independent inventory identity; source multiplayer item use is not enabled.",
            "Anor Londo departure, later quest branches, hostility combat and source summons remain unported. Native interaction, audio, inventory, save/reload and pose require gameplay validation."
        ]);
    }
}
