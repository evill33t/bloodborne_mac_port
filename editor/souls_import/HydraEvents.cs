// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// Source Basin head ownership on the native event VM. The bodies, masks,
// RagdollParam group identities and owned TAE event-action carriers are supplied
// by the character adapter; no generic boss admission/fog/bar is invented.
internal static class HydraEvents {
    internal const int FirstEvent=12416000;
    internal const int Body=4048000,FirstHead=4048011;
    internal static int Defeat=>MapNpcQuestState.Flag(11200801);
    internal sealed record Head(int Slot,int SourcePart,short Group,int HP,int DisplayMask,int CollisionMask,int SourceSocket,int TargetSocket,int Effect,int Flag,int Entity);
    internal sealed record Result(byte[] Script,IReadOnlyList<Head> Heads,string SourceEventHash,IReadOnlyList<string> Limits);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Hydra event: "+why);}
    static byte[] Args(params object[] a) {
        using var s=new MemoryStream();using var w=new BinaryWriter(s);
        foreach(var v in a){int width=v is byte or sbyte?1:v is short or ushort?2:4;while(s.Position%width!=0)w.Write((byte)0);switch(v){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case short n:w.Write(n);break;case ushort n:w.Write(n);break;case int n:w.Write(n);break;case uint n:w.Write(n);break;case float n:w.Write(n);break;default:throw new InvalidDataException("Hydra event argument type");}}
        while(s.Position%4!=0)w.Write((byte)0);return s.ToArray();
    }
    static EMEVD.Instruction I(int bank,int id,params object[] a)=>new(bank,id,Args(a));
    internal static Result Apply(byte[] destination,byte[] source,IReadOnlyDictionary<int,int> effects,int rewardLot=-1) {
        var input=EMEVD.Read(source);Need(input.Format==EMEVD.Game.DarkSouls1,"source event VM");
        var sourceHead=input.Events.Single(e=>e.ID==11205300);Need(sourceHead.RestBehavior==EMEVD.Event.RestBehaviorType.Restart&&sourceHead.Instructions.Count==22,"source head event contract changed");
        var calls=input.Events.Single(e=>e.ID==0).Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&BitConverter.ToInt32(i.ArgData,4)==11205300).OrderBy(i=>BitConverter.ToInt32(i.ArgData)).ToArray();
        Need(calls.Length==7,"source head population changed");
        var script=EMEVD.Read(destination);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=FirstEvent&&e.ID<FirstEvent+10),"native VM/event namespace");
        var ctor=script.Events.Single(e=>e.ID==0);var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}
        var initialization=new List<EMEVD.Instruction>{I(2004,10,Body,(byte)0),I(2004,31,Body,(byte)1),I(1003,1,(byte)1,(byte)0,(byte)0,Defeat),I(2004,5,Body,(byte)0)};
        var heads=new List<Head>();
        for(int slot=0;slot<7;slot++) {
            var b=calls[slot].ArgData;short group=(short)(slot==6?8:slot+1),part=(short)(3530+slot);int actor=1200011+slot,socket=91+slot;
            Need(b.AsSpan().SequenceEqual(Args(slot,11205300,group,part,(int)part,actor,socket,(byte)slot,(byte)(slot+1),5430+slot)),"source head call changed: "+slot);
            // Resolve the source instruction parameters, then check the actual
            // create-part command, not only the constructor's argument names.
            var create=sourceHead.Instructions[7].ArgData.ToArray();
            foreach(var p in sourceHead.Parameters.Where(p=>p.InstructionIndex==7))Array.Copy(b,checked((int)p.SourceStartByte+8),create,checked((int)p.TargetStartByte),checked((int)p.ByteCount));
            Need(sourceHead.Instructions[7].Bank==2004&&sourceHead.Instructions[7].ID==22&&create.AsSpan().SequenceEqual(Args(1200010,part,group,176,1f,1f,(byte)0,(byte)0)),"source hurtbox ownership changed");
            int entity=FirstHead+slot,flag=MapNpcQuestState.Flag(11205300+slot),effect=effects[5430+slot],id=FirstEvent+slot;
            var h=new Head(slot,part,group,176,slot,slot+1,socket,500+socket,effect,flag,entity);heads.Add(h);
            initialization.AddRange([I(2004,5,entity,(byte)0),I(2004,1,entity,(byte)0),I(2004,10,entity,(byte)0),I(2004,31,entity,(byte)1),I(2004,15,entity,(byte)1)]);
            var e=new EMEVD.Event(id,EMEVD.Event.RestBehaviorType.Restart);
            e.Instructions.AddRange([
                I(1003,2,(byte)0,(byte)1,(byte)0,Defeat),
                I(4,7,(sbyte)0,Body,(byte)1),
                I(1003,1,(byte)4,(byte)0,(byte)0,flag),
                I(2004,26,Body,(byte)slot,(byte)0),I(2004,27,Body,(byte)(slot+1),(byte)1),I(2004,8,Body,effect,(byte)0),I(1000,4,(byte)0),
                I(2004,22,Body,part,group,176,1f,1f,(byte)0,(byte)0),
                I(4,6,(sbyte)1,Body,(int)part,0,(sbyte)5),I(4,2,(sbyte)2,Body,(sbyte)5,0f),
                I(0,0,(sbyte)-1,(byte)1,(sbyte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)2),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(1000,8,(byte)0,(byte)1,(sbyte)2),
                I(2004,43,Body,(byte)0),I(2004,42,entity,(byte)2,Body,500+socket,Body),I(2004,5,entity,(byte)1),
                I(2003,18,entity,7005,(byte)0,(byte)0,(byte)0),I(2003,18,Body,7005,(byte)0,(byte)0,(byte)0),
                I(2004,26,Body,(byte)slot,(byte)0),I(2004,27,Body,(byte)(slot+1),(byte)1),I(2004,8,Body,effect,(byte)0),I(2003,2,flag,(byte)1),
                I(1001,0,2.6f),I(2004,4,entity,(byte)0),I(1001,0,2.4f),I(2004,5,entity,(byte)0)
            ]);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,id,0));
        }
        // Death can come from body damage or all seven authored head parts.
        // Head8100 reaches its source death gate at2.533s; the all-head branch
        // waits that interval before killing the body and awarding its lot.
        var death=new EMEVD.Event(FirstEvent+7,EMEVD.Event.RestBehaviorType.Restart);
        death.Instructions.AddRange([I(1003,1,(byte)2,(byte)0,(byte)0,Defeat),I(2004,5,Body,(byte)0),I(1000,4,(byte)0),I(4,0,(sbyte)-1,Body,(byte)1)]);
        foreach(var h in heads)death.Instructions.Add(I(3,0,(sbyte)1,(byte)1,(byte)0,h.Flag));
        death.Instructions.AddRange([I(0,0,(sbyte)-1,(byte)1,(sbyte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(1001,0,2.6f),I(2004,4,Body,(byte)1),I(2003,2,Defeat,(byte)1)]);
        if(rewardLot>=0)death.Instructions.Add(I(2003,36,rewardLot));
        script.Events.Add(death);ctor.Instructions.Add(I(2000,0,0,FirstEvent+7,0));
        foreach(var p in pre.Parameters)p.InstructionIndex+=initialization.Count;pre.Instructions.InsertRange(0,initialization);
        var result=script.Write(DCX.Type.DCX_DFLT_10000_44_9);_=EMEVD.Read(result);
        return new(result,heads,Convert.ToHexStringLower(SHA256.HashData(source)),[
            "Seven source176HP part groups own the original head hurtboxes; the seventh uses group8. Masks, sever sockets, source recoil and severed-head playback use the explicit native carriers.",
            "Body defeat and individual cuts persist in owned native flag storage. A complete head break removes its collision mask and source attack-judge branch. Severed visual actors are killed at the source death-gate time and removed at5s.",
            "This encounter has no source fog wall, boss health bar or boss music. Native combat, water VFX and solver differences are reported by the character stages. Full native contact/sever/defeat behavior requires gameplay verification."]);
    }
}
