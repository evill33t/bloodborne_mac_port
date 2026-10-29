// SPDX-License-Identifier: GPL-2.0-or-later
using System.Numerics;

internal static class MapCollisionBindingsTests {
    static void Check(bool ok,string why){if(!ok)throw new Exception(why);}
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Unpaired source regions accepted");}
    internal static void Run() {
        SourceCollision.Part Part(string name,string layer,int count,float x)=>new(name,"h0000",8,layer,
            [new(1,Enumerable.Range(0,count).Select(i=>new MapCollision.Triangle(new(x+i,0,0),new(x+i,0,1),new(x+i+1,0,0))).ToArray())]);
        var source=new SourceCollision.Result([Part("exterior","h",12,0),Part("exterior","l",8,0),Part("interior","h",20,100),Part("interior","l",10,100)]);
        var chunks=MapCollisionBindings.Partition(source,7);
        Check(chunks.Count==7&&chunks.Sum(c=>c.High.Count)==32&&chunks.Sum(c=>c.Low.Count)==18,"Collision partition lost H/L triangles");
        foreach(var chunk in chunks) {
            Check(chunk.High.Count>0&&chunk.Low.Count>0,"A native chunk has an empty H/L layer");
            Check(chunk.High.Concat(chunk.Low).All(t=>(t.A.X<100)==(chunk.SourceName=="exterior")),"A chunk mixed distinct source lighting regions");
        }
        Reject(()=>MapCollisionBindings.Partition(source,1));
        Reject(()=>MapCollisionBindings.Partition(new([Part("exterior","h",12,0),Part("interior","l",10,100)]),7));
        // Fine/coarse layers have opposite triangle densities along this ramp.
        // Separate median cuts used to pair opposite ends of the same placement.
        var ramp=new SourceCollision.Result([new("ramp","h",8,"h",[new(1,Part("ramp","h",20,0).Surfaces[0].Triangles.Concat(Part("ramp","h",2,100).Surfaces[0].Triangles).ToArray())]),
            new("ramp","l",8,"l",[new(1,Part("ramp","l",2,0).Surfaces[0].Triangles.Concat(Part("ramp","l",20,100).Surfaces[0].Triangles).ToArray())])]);
        var paired=MapCollisionBindings.Partition(ramp,2);
        Check(paired[0].High.Concat(paired[0].Low).All(t=>t.A.X<50)&&paired[1].High.Concat(paired[1].Low).All(t=>t.A.X>50),"Fine/coarse ramp sections occupy different ends of a native collision record");
        Console.WriteLine("PASS: native collision chunks retain paired source H/L lighting regions and all triangles");
    }
}
