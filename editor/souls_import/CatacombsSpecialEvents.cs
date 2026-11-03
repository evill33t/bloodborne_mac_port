// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
internal static class CatacombsSpecialEvents {
    internal const int First=12416400;
    internal static int Flag(int sourceSlot)=>sourceSlot is >=6 and <=9?52411130+sourceSlot:throw new ArgumentOutOfRangeException(nameof(sourceSlot));
    static EMEVD.Instruction I(int bank,int id,params object[] args)=>CatacombsCreaturesEvents.I(bank,id,args);
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs special events: "+why);}
    internal static byte[] Apply(byte[] bytes,CatacombsSpecialSource.Plan plan,IReadOnlyList<MapEnemySpawns.Spawn> spawns,float lizardHideDelay) {
        Need(float.IsFinite(lizardHideDelay)&&lizardHideDelay is >=0 and <=10,"lizard fade delay");var script=EMEVD.Read(bytes);Need(script.Format==EMEVD.Game.Bloodborne&&!script.Events.Any(e=>e.ID>=First&&e.ID<First+100),"native event reservation");var ctor=script.Events.Single(e=>e.ID==0);var actors=spawns.ToDictionary(s=>s.SourceEntityID);int next=First;
        void Add(EMEVD.Event e){script.Events.Add(e);ctor.Instructions.Add(I(2000,0,0,(int)e.ID,0));}
        foreach(var d in plan.Deaths) {
            int actor=actors[d.SourceActor].EntityID,flag=Flag(d.Slot);var e=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
            e.Instructions.AddRange([I(1003,101,(byte)0,(byte)0,(byte)0,flag),I(2004,5,actor,(byte)0),I(2004,1,actor,(byte)0),I(1000,4,(byte)0),I(1014,0),I(4,0,(sbyte)0,actor,(byte)1),I(2003,2,flag,(byte)1)]);Add(e);
        }
        foreach(var wisp in spawns.Where(s=>s.SourceModel=="c3501").OrderBy(s=>s.SourceEntityID)) {
            int actor=wisp.EntityID;var e=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
            e.Instructions.AddRange([I(4,0,(sbyte)3,actor,(byte)1),I(1000,2,(byte)0,(byte)1,(sbyte)3),I(2004,10,actor,(byte)0),I(2004,5,actor,(byte)1),I(2004,1,actor,(byte)1),
                I(4,8,(sbyte)1,actor,400,(byte)1),I(4,0,(sbyte)2,actor,(byte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)2),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(1000,8,(byte)0,(byte)1,(sbyte)2),
                I(2004,1,actor,(byte)0),I(2004,5,actor,(byte)0)]);Add(e);
        }
        foreach(var lizard in spawns.Where(s=>s.SourceModel=="c3300").OrderBy(s=>s.SourceEntityID)) {
            int actor=lizard.EntityID,flag=Flag(plan.Deaths.Single(d=>d.SourceActor==lizard.SourceEntityID).Slot);var e=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
            e.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,flag),I(2004,5,actor,(byte)1),I(2004,1,actor,(byte)1),I(4,8,(sbyte)1,actor,1000,(byte)1),I(4,0,(sbyte)2,actor,(byte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)1),I(0,0,(sbyte)-1,(byte)1,(sbyte)2),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(1000,8,(byte)0,(byte)1,(sbyte)2)]);
            if(lizardHideDelay>0)e.Instructions.Add(I(1001,0,lizardHideDelay));e.Instructions.AddRange([I(2004,1,actor,(byte)0),I(2004,5,actor,(byte)0)]);Add(e);
        }
        int demon=actors[1300300].EntityID;var sleep=new EMEVD.Event(next++,EMEVD.Event.RestBehaviorType.Restart);
        sleep.Instructions.AddRange([I(1003,2,(byte)0,(byte)1,(byte)0,Flag(8)),I(2004,1,demon,(byte)0),I(2004,9,demon,7000,-1,-1,-1,-1),I(2003,18,demon,7000,(byte)1,(byte)0,(byte)0),I(3,3,(sbyte)-1,(byte)1,demon,10000,5f),I(4,1,(sbyte)-1,demon,10000),I(0,0,(sbyte)0,(byte)1,(sbyte)-1),I(2004,9,demon,-1,-1,-1,-1,-1),I(2004,1,demon,(byte)1)]);Add(sleep);
        var pre=script.Events.SingleOrDefault(e=>e.ID==50);if(pre==null){pre=new(50);script.Events.Add(pre);}var initial=spawns.Where(s=>s.SourceModel=="c3501").Select(s=>I(2004,10,s.EntityID,(byte)0)).ToArray();foreach(var p in pre.Parameters)p.InstructionIndex+=initial.Length;pre.Instructions.InsertRange(0,initial);
        Need(next-First==19,"special controller population changed");return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    internal static byte[] Rewards(byte[] bytes,CatacombsSpecialSource.Plan plan,IReadOnlyDictionary<int,int> lots) {
        var script=EMEVD.Read(bytes);for(int n=0;n<plan.Deaths.Length;n++){var d=plan.Deaths[n];if(d.Lot<=0||!lots.TryGetValue(d.Lot,out int target))continue;var e=script.Events.Single(e=>e.ID==First+n);Need(!e.Instructions.Any(i=>i.Bank==2003&&i.ID==36),"special reward already attached");e.Instructions.Add(I(2003,36,target));}return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
