// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
internal static class PinwheelEvents {
    internal const int First=12416800,RewardFlag=52411118,ImmediateRewardFlag=52411119,Name=9008880;
    internal const int Active=52411247,Admitted=52411248;
    internal static int Defeat=>MapNpcQuestState.Flag(6);
    internal static int Intro=>MapNpcQuestState.Flag(11300000);
    internal static readonly int[] ReservedFlags=Enumerable.Range(52411200,64).Append(RewardFlag).Append(ImmediateRewardFlag).ToArray();
    internal static readonly IReadOnlyDictionary<int,int> EventIDs=new Dictionary<int,int>{{11305350,First+10},{11305370,First+11},{11305250,First+12},{11305396,First+13},{11305397,First+14},{11305398,First+15},{11305330,First+16}};
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Pinwheel events: "+why);}
    internal static byte[] Args(params object[] values) {
        using var s=new MemoryStream();using var w=new BinaryWriter(s);foreach(var v in values){int width=v is byte or sbyte?1:v is short or ushort?2:4;while(s.Position%width!=0)w.Write((byte)0);switch(v){case byte n:w.Write(n);break;case sbyte n:w.Write(n);break;case short n:w.Write(n);break;case ushort n:w.Write(n);break;case int n:w.Write(n);break;case uint n:w.Write(n);break;case float n:w.Write(n);break;default:throw new InvalidDataException("event argument type");}}while(s.Position%4!=0)w.Write((byte)0);return s.ToArray();
    }
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>new(bank,id,Args(args));
    static IReadOnlyDictionary<int,int> Flags() {
        var flags=new Dictionary<int,int>{{11305310,52411244},{11305329,52411245},{11305399,52411246},{11305392,Active},{11305390,Admitted}};
        foreach(var(first,count,target)in new[]{(11305251,14,52411200),(11305300,7,52411220),(11305320,7,52411230)})for(int i=0;i<count;i++)flags.Add(first+i,target+i);return flags;
    }
    internal static byte[] Apply(byte[] destination,byte[] source,IReadOnlyDictionary<int,int> regions,BossFogWalls.Binding fog) {
        Need(Convert.ToHexStringLower(SHA256.HashData(source))=="691516e02235c8770629d032b6512e2ce1e14844487b198a1ee88756207431ba","source encounter resource changed");
        var original=EMEVD.Read(source);var script=EMEVD.Read(destination);Need(original.Format==EMEVD.Game.DarkSouls1&&script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=First&&e.ID<First+100),"VM or event reservation");
        var ctor=script.Events.Single(e=>e.ID==0);var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var init=new List<EMEVD.Instruction>{I(2003,22,52411200,52411263,(byte)0),I(2003,11,(sbyte)0,Pinwheel.Body,(short)0,Name)};
        for(int i=0;i<15;i++){init.AddRange([I(2004,5,Pinwheel.Body+i,(byte)0),I(2004,1,Pinwheel.Body+i,(byte)(i==0?0:1)),I(2004,30,Pinwheel.Body+i,(byte)0)]);if(i>0)init.Add(I(2004,12,Pinwheel.Body+i,(byte)1));}
        foreach(var p in pre.Parameters)p.InstructionIndex+=init.Count;pre.Instructions.InsertRange(0,init);
        void Add(EMEVD.Event e,bool run=true){script.Events.Add(e);if(run)ctor.Instructions.Add(I(2000,0,0,(int)e.ID,0));}
        var start=new EMEVD.Event(First,EMEVD.Event.RestBehaviorType.Restart);
        start.Instructions.AddRange([I(1003,1,(byte)4,(byte)0,(byte)0,Defeat),I(2005,3,fog.ObjectEntity,(byte)0),I(2006,1,fog.SfxEntity,(byte)1),I(2004,5,Pinwheel.Body,(byte)0),I(1000,4,(byte)0),I(2005,3,fog.ObjectEntity,(byte)1),I(2006,2,fog.SfxEntity),I(3,0,(sbyte)1,(byte)1,(byte)0,Admitted),I(3,2,(sbyte)1,(byte)1,10000,regions[1302999]),I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(2004,5,Pinwheel.Body,(byte)1),I(2004,1,Pinwheel.Body,(byte)1),I(2004,15,Pinwheel.Body,(byte)0),I(2004,13,Pinwheel.Body,regions[1302600]),I(2003,2,Intro,(byte)1),I(2003,2,MapNpcQuestState.Flag(11300005),(byte)1),I(2003,2,Active,(byte)1),I(2004,20,Pinwheel.Body),I(2003,11,(sbyte)1,Pinwheel.Body,(short)0,Name)]);Add(start);
        var traverse=new EMEVD.Event(First+2,EMEVD.Event.RestBehaviorType.Restart);
        traverse.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,Defeat),I(3,2,(sbyte)1,(byte)1,10000,regions[1302998]),I(3,24,(sbyte)1,BossFogWalls.ActionButtonID,fog.ObjectEntity),I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(2004,14,10000,regions[1302997],101130,(byte)0),I(3,2,(sbyte)-1,(byte)1,10000,regions[1302996]),I(1,0,(sbyte)-1,2f),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(3,2,(sbyte)2,(byte)1,10000,regions[1302996]),I(1000,2,(byte)1,(byte)0,(sbyte)2),I(2003,2,Admitted,(byte)1),I(1000,4,(byte)1)]);Add(traverse);
        var death=new EMEVD.Event(First+1,EMEVD.Event.RestBehaviorType.Restart);death.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,Defeat),I(3,0,(sbyte)1,(byte)1,(byte)0,Active),I(4,0,(sbyte)1,Pinwheel.Body,(byte)1),I(0,0,(sbyte)0,(byte)1,(sbyte)1),I(2003,12,Pinwheel.Body),I(2003,2,Defeat,(byte)1),I(2003,2,Active,(byte)0),I(2003,11,(sbyte)0,Pinwheel.Body,(short)0,Name),I(2005,3,fog.ObjectEntity,(byte)0),I(2006,1,fog.SfxEntity,(byte)1)]);for(int i=1;i<15;i++)death.Instructions.AddRange([I(2004,12,Pinwheel.Body+i,(byte)0),I(2004,4,Pinwheel.Body+i,(byte)0),I(2004,5,Pinwheel.Body+i,(byte)0)]);Add(death);
        var reward=new EMEVD.Event(First+3,EMEVD.Event.RestBehaviorType.Restart);reward.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,RewardFlag),I(3,0,(sbyte)0,(byte)1,(byte)0,Defeat),I(2003,2,RewardFlag,(byte)1)]);Add(reward);
        var replacements=Flags().Concat(regions).Concat(EventIDs).ToDictionary(p=>p.Key,p=>p.Value);for(int i=0;i<15;i++)replacements.Add(1300800+i,Pinwheel.Body+i);replacements.Add(5450,888299);
        EMEVD.Instruction Translate(EMEVD.Instruction from) {
            var args=from.ArgData.ToArray();for(int n=0;n+4<=args.Length;n+=4){int value=BitConverter.ToInt32(args,n);if(replacements.TryGetValue(value,out int target))BitConverter.GetBytes(target).CopyTo(args,n);}
            // Native adds a part-HP flag to SetSpEffect; the source's fields
            // and parameter offsets remain unchanged. Its zero padding in
            // SetUpdateRate supplies the native frequency byte0 (every frame).
            if(from.Bank==2004&&from.ID==8){Need(args.Length==8,"source heal ABI");Array.Resize(ref args,12);}
            return new(from.Bank,from.ID,args){Layer=from.Layer};
        }
        foreach(var(sourceId,id)in EventIDs) {
            var from=original.Events.Single(e=>e.ID==sourceId);var e=new EMEVD.Event(id,from.RestBehavior);e.Instructions.AddRange(from.Instructions.Select(Translate));
            foreach(var p in from.Parameters)e.Parameters.Add(new EMEVD.Parameter(p.InstructionIndex,p.TargetStartByte,p.SourceStartByte,p.ByteCount){UnkID=p.UnkID});
            Add(e,false);
        }
        var calls=original.Events.Single(e=>e.ID==0).Instructions.Where(i=>i.Bank==2000&&i.ID==0&&i.ArgData.Length>=8&&EventIDs.ContainsKey(BitConverter.ToInt32(i.ArgData,4))).ToArray();
        Need(calls.Length==25&&calls.Count(i=>BitConverter.ToInt32(i.ArgData,4)==11305350)==7&&calls.Count(i=>BitConverter.ToInt32(i.ArgData,4)==11305370)==14,"source phase/helper controllers changed");ctor.Instructions.AddRange(calls.Select(Translate));
        var bytes=script.Write(DCX.Type.DCX_DFLT_10000_44_9);_=EMEVD.Read(bytes);return bytes;
    }
    internal static byte[] Rewards(byte[] bytes,IReadOnlyDictionary<int,int> lots) {
        Need(new[]{2550,9000,9030}.All(id=>lots.TryGetValue(id,out int native)&&native>0),"converted source reward lot missing");
        var script=EMEVD.Read(bytes);var e=script.Events.Single(e=>e.ID==First+3);Need(e.Instructions.Count==3&&!e.Instructions.Any(i=>i.Bank==2003&&i.ID is 12 or 36),"reward already attached or unexpected controller");
        // Keep immediate quest reward and delayed consumable completion in
        // separate persistent bits. A reload during the source five-second
        // delay resumes consumables without replaying Rite or boss-clear souls.
        e.Instructions.InsertRange(2,[I(1003,1,(byte)2,(byte)1,(byte)0,ImmediateRewardFlag),I(2003,36,lots[2550]),I(2003,2,ImmediateRewardFlag,(byte)1),I(1001,0,5f),I(2003,36,lots[9000]),I(2003,36,lots[9030])]);
        return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
