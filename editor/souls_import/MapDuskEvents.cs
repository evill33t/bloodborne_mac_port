// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

internal static class MapDuskEvents {
    internal const int FirstEvent=12416020;
    internal sealed record Result(byte[] Script,string SourceSHA256,IReadOnlyList<int> Events,IReadOnlyList<string> Limitations);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Dusk source events: "+why);}
    static int Flag(int source)=>MapNpcQuestState.Flag(source);
    static EMEVD.Instruction I(int bank,int id,params object[] args) {
        using var stream=new MemoryStream();using var w=new BinaryWriter(stream);
        foreach(var a in args){int width=a is byte or sbyte?1:a is short or ushort?2:4;while(stream.Position%width!=0)w.Write((byte)0);switch(a){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case short n:w.Write(n);break;case ushort n:w.Write(n);break;case int n:w.Write(n);break;case float n:w.Write(n);break;default:throw new InvalidDataException("Dusk instruction scalar");}}
        while(stream.Position%4!=0)w.Write((byte)0);return new(bank,id,stream.ToArray());
    }
    static IEnumerable<EMEVD.Instruction> State(int next)=>Enumerable.Range(1120,20).Select(f=>I(2003,2,Flag(f),(byte)0)).Append(I(2003,2,Flag(next),(byte)1));
    internal static Result Apply(byte[] native,byte[] source,int dusk,int golden,int goldenSocket,IReadOnlyList<MapDuskAnimations.Motion> motions) {
        string hash=Convert.ToHexStringLower(SHA256.HashData(source));Need(hash=="2e13def5aa25a2d4b500906c2bd08d329da58a3aa9521325dbd38943adb00b6e","audited source map event file changed");
        var original=EMEVD.Read(source);Need(original.Events.Single(e=>e.ID==11200800).Instructions.Count==9&&original.Events.Single(e=>e.ID==11200530).Instructions.Count==10,"source golem/rescue contract changed");
        var script=EMEVD.Read(native);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+6),"native event VM/owned namespace");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var ctor=script.Events.Single(e=>e.ID==0);
        var init=new[]{I(2004,5,golden,(byte)0),I(2004,1,dusk,(byte)0),I(2004,5,dusk,(byte)0),I(1003,1,(byte)1,(byte)0,(byte)0,Flag(1121)),I(2004,5,dusk,(byte)1)};
        foreach(var p in pre.Parameters)p.InstructionIndex+=init.Length;pre.Instructions.InsertRange(0,init);
        var ids=new List<int>();
        EMEVD.Event Event(int offset){var e=new EMEVD.Event(FirstEvent+offset,EMEVD.Event.RestBehaviorType.Restart);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));ids.Add((int)e.ID);return e;}
        // This is the exact source's already-Hydra-dead entry branch. The
        // engine-managed703 condition for a visit begun before Hydra dies is
        // intentionally not fabricated; that path waits for a map reload.
        var gate=Event(0);gate.Instructions.AddRange([
            I(2004,5,golden,(byte)0),I(1003,2,(byte)0,(byte)1,(byte)0,Flag(11200800)),
            I(1003,2,(byte)0,(byte)0,(byte)0,Flag(11200801)),I(2004,5,golden,(byte)1),
            I(4,0,(sbyte)0,golden,(byte)1),I(2003,2,Flag(11200800),(byte)1)]);
        var rescue=Event(1);
        rescue.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,Flag(1120)),I(3,0,(sbyte)1,(byte)1,(byte)0,Flag(11200800)),I(0,0,(sbyte)0,(byte)1,(sbyte)1)]);
        rescue.Instructions.AddRange(State(1121));
        rescue.Instructions.AddRange([I(2004,42,dusk,(byte)2,golden,goldenSocket,golden),I(2004,5,dusk,(byte)1),I(1001,0,.5f),
            I(2003,18,dusk,7000,(byte)0,(byte)0,(byte)0)]);
        foreach(var choice in new[]{(Offset:2,Completion:11200590,Next:1122),(Offset:3,Completion:11200591,Next:1126)}) {
            var leave=Event(choice.Offset);leave.Instructions.AddRange([I(3,0,(sbyte)1,(byte)1,(byte)0,Flag(1121)),I(3,0,(sbyte)1,(byte)1,(byte)0,Flag(choice.Completion)),I(0,0,(sbyte)0,(byte)1,(sbyte)1)]);
            leave.Instructions.AddRange(State(choice.Next));
            foreach(int action in new[]{7001,7010})leave.Instructions.AddRange([I(2003,18,dusk,action,(byte)0,(byte)0,(byte)0),I(1001,0,motions.Single(m=>m.Native==action).Duration)]);
            leave.Instructions.Add(I(2004,5,dusk,(byte)0));
        }
        var death=Event(4);death.Instructions.AddRange([I(3,0,(sbyte)0,(byte)1,(byte)0,Flag(1121)),I(4,0,(sbyte)0,dusk,(byte)1)]);death.Instructions.AddRange(State(1125));
        var output=script.Write(DCX.Type.DCX_DFLT_10000_44_9);_=EMEVD.Read(output);
        return new(output,hash,ids,[
            "Golden golem presence follows the source already-Hydra-dead map-entry branch. For a visit entered with the Hydra alive, the golem remains hidden until reloading; engine-managed source703 semantics are unproved and are not set globally.",
            "Actual golem death releases Dusk at its retained source101 socket. First rescue conversation, interrupted-offer recovery, yes/no, source1121→1122/1126, departure and rescue-state death persist in owned flags.",
            "Summon signs, source spell shop, gesture, later DLC/prisoner encounters and attack/hostility dialogue are separate unsupported stages. These events alone do not supply golden-golem combat."]);
    }
}
