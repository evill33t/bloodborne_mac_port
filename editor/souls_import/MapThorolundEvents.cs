// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// Firelink's local party progression. No Tomb rescue, later Rhea stages or
// global world-visit counter is fabricated by this adapter.
internal static class MapThorolundEvents {
    internal const int FirstEvent=12416640;
    internal static readonly (int Source,int Entity,int Visible,int First,int Last,int Death)[] Party=[
        (6070,4026070,1171,1170,1180,1177),(6090,4026090,1211,1210,1219,1214),(6100,4026100,1221,1220,1229,1224)];
    static int F(int source)=>MapNpcQuestState.Flag(source);
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,args);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Thorolund quest events: "+why);}
    static void Set(List<EMEVD.Instruction> code,int source,bool on=true)=>code.Add(I(2003,2,F(source),(byte)(on?1:0)));
    static void Range(List<EMEVD.Instruction> code,int first,int last,int next){for(int f=first;f<=last;f++)Set(code,f,false);Set(code,next);}
    static void Flag(List<EMEVD.Instruction> code,sbyte group,int source,bool on=true)=>code.Add(I(3,0,group,(byte)(on?1:0),(byte)0,F(source)));
    static void Healthy(List<EMEVD.Instruction> code,sbyte group) {
        foreach(var range in new[]{(1176,1181),(1195,1198),(1213,1215),(1223,1225)})for(int f=range.Item1;f<=range.Item2;f++)Flag(code,group,f,false);
        foreach(int f in new[]{1171,1202,1211,1221})Flag(code,group,f);
    }
    sealed class Branches(List<EMEVD.Instruction> code) {
        readonly Dictionary<string,int> labels=[];readonly List<(int At,int Flag,string Label)> pending=[];
        internal void Label(string label)=>labels.Add(label,code.Count);
        internal void If(int source,string label){pending.Add((code.Count,source,label));code.Add(I(1003,1,(byte)0,(byte)1,(byte)0,F(source)));}
        internal void Finish(){foreach(var p in pending){int skip=labels[p.Label]-p.At-1;Need(skip is >=0 and <=255,"native branch exceeds byte span");code[p.At]=I(1003,1,(byte)skip,(byte)1,(byte)0,F(p.Flag));}}
    }
    internal static byte[] Apply(byte[] native,byte[] source,int petrusEntity) {
        Need(petrusEntity>0,"missing existing Petrus actor");
        Need(Convert.ToHexStringLower(SHA256.HashData(source))=="b519c70b1d3bdbaf7deb0472b47355b827cbae5f61b91c9edd915e41cec0d346","source Firelink script changed");
        var original=EMEVD.Read(source);var arrival=original.Events.Single(e=>e.ID==11020559);
        EMEVD.Instruction[] first=[I(3,0,(sbyte)1,(byte)1,(byte)0,1170),I(3,0,(sbyte)1,(byte)1,(byte)0,11010902),I(0,0,(sbyte)0,(byte)1,(sbyte)1)];
        Need(arrival.Instructions.Count==12&&arrival.Instructions.Take(3).Zip(first).All(p=>p.First.Bank==p.Second.Bank&&p.First.ID==p.Second.ID&&p.First.ArgData.SequenceEqual(p.Second.ArgData)),"source after-Capra arrival changed");
        var departure=original.Events.Single(e=>e.ID==11020560);
        Need(departure.Instructions.Count==34&&departure.Instructions[19].Bank==1000&&departure.Instructions[19].ID==8&&departure.Instructions[19].ArgData.SequenceEqual(I(1000,8,(byte)0,(byte)1,(sbyte)1).ArgData),"source deferred departure contract changed");
        var script=EMEVD.Read(native);Need(script.Format==EMEVD.Game.Bloodborne&&script.Events.All(e=>e.ID<FirstEvent||e.ID>=FirstEvent+20),"owned native event namespace occupied");
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}
        // Append after the shared initializer. These gates must see source
        // common's fresh1170/1210/1220 states on the very first load.
        foreach(var p in Party)pre.Instructions.AddRange([I(2004,1,p.Entity,(byte)0),I(2004,5,p.Entity,(byte)0),I(1003,1,(byte)1,(byte)0,(byte)0,F(p.Visible)),I(2004,5,p.Entity,(byte)1)]);
        pre.Instructions.Add(I(2004,1,petrusEntity,(byte)0));
        var ctor=script.Events.Single(e=>e.ID==0);
        EMEVD.Event Event(int offset,EMEVD.Event.RestBehaviorType rest=EMEVD.Event.RestBehaviorType.Default){var e=new EMEVD.Event(FirstEvent+offset,rest);script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,checked((int)e.ID),0));return e;}
        var a=Event(0).Instructions;Flag(a,1,1170);Flag(a,1,11010902);a.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)1));
        Range(a,1170,1189,1171);Range(a,1210,1219,1211);Range(a,1220,1229,1221);foreach(var p in Party)a.Add(I(2004,5,p.Entity,(byte)1));
        var stage=Event(1).Instructions;var branch=new Branches(stage);
        // The source departure event's first farewell run only completes its
        // own event flag. The next load performs departure. Keep that distinct
        // persistent source identity rather than using our native event ID.
        branch.If(1192,"petrus return");stage.Add(I(1003,2,(byte)0,(byte)1,(byte)0,F(11020693)));branch.If(11020560,"depart");
        Healthy(stage,1);Flag(stage,-1,11020600);Flag(stage,-1,11300005);stage.Add(I(0,0,(sbyte)1,(byte)1,(sbyte)-1));stage.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)1));
        Set(stage,11020560);stage.Add(I(1003,2,(byte)0,(byte)0,(byte)0,F(11300005)));
        branch.Label("depart");Healthy(stage,2);stage.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)2));
        Set(stage,11020693);Range(stage,1170,1189,1173);Range(stage,1190,1209,1192);Range(stage,1210,1219,1216);Range(stage,1220,1229,1226);
        foreach(var p in Party)stage.Add(I(2004,5,p.Entity,(byte)0));stage.Add(I(2004,5,petrusEntity,(byte)0));
        // Source11020564 is initialized after11020560 and records its first
        //1192 visit. Combining those bounded stages removes scheduler races.
        Set(stage,11020692);stage.Add(I(1000,4,(byte)0));
        branch.Label("petrus return");stage.Add(I(1003,2,(byte)0,(byte)1,(byte)0,F(1195)));stage.Add(I(1003,2,(byte)0,(byte)1,(byte)0,F(1197)));branch.If(11020692,"return now");Set(stage,11020692);stage.Add(I(1000,4,(byte)0));
        branch.Label("return now");Range(stage,1190,1209,1193);stage.Add(I(2004,5,petrusEntity,(byte)1));branch.Finish();
        for(int index=0;index<Party.Length;index++) {
            var p=Party[index];var visible=Event(2+index*2,EMEVD.Event.RestBehaviorType.Restart).Instructions;
            Flag(visible,0,p.Visible);visible.Add(I(2004,5,p.Entity,(byte)1));Flag(visible,0,p.Visible,false);
            visible.Add(I(1003,2,(byte)0,(byte)1,(byte)0,F(p.Death)));visible.Add(I(2004,5,p.Entity,(byte)0));visible.Add(I(1000,4,(byte)1));
            var death=Event(3+index*2).Instructions;Flag(death,1,p.Visible);death.Add(I(4,0,(sbyte)1,p.Entity,(byte)1));death.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)1));Range(death,p.First,p.Last,p.Death);
        }
        // Source11020569 resolves a death outside1194/1195/1196 to1198.
        // Only the two local live stages are adapted; the shared availability
        // event owns disabling the actor after this persistent state changes.
        var petrusDeath=Event(8).Instructions;Flag(petrusDeath,-1,1202);Flag(petrusDeath,-1,1193);petrusDeath.Add(I(0,0,(sbyte)1,(byte)1,(sbyte)-1));
        petrusDeath.Add(I(4,0,(sbyte)1,petrusEntity,(byte)1));petrusDeath.Add(I(0,0,(sbyte)0,(byte)1,(sbyte)1));Range(petrusDeath,1190,1209,1198);
        return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
