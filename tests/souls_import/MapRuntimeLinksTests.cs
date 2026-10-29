// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class MapRuntimeLinksTests {
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    internal static void Run() {
        var source=new MSB1();var link=new MSB1.Part.ConnectCollision{Name="to_burg",CollisionName="source_floor"};
        byte[] wildcard=[10,1,255,255];Array.Copy(wildcard,link.MapID,4);source.Parts.ConnectCollisions.Add(link);
        var native=new MSBB();var model=new MSBB.Model.Collision{Name="h000001"};native.Models.Collisions.Add(model);
        var a=new MSBB.Part.Collision{Name="floor_a",ModelName=model.Name,Position=Vector3.Zero};
        var b=(MSBB.Part.Collision)a.DeepCopy();b.Name="floor_b";native.Parts.Collisions.AddRange([a,b]);
        native.Parts.ConnectCollisions.Add(new(){Name="stale_native",ModelName=model.Name,CollisionName=a.Name});
        var chunks=new[]{new MapCollisionBindings.Chunk("source_floor",[],[]),new MapCollisionBindings.Chunk("source_floor",[],[])};
        var result=MapRuntimeLinks.Apply(native,source,chunks,[a,b],new Dictionary<string,string>{{"m10_01_00_00","m24_00_00_00"}});
        Check(result.RemovedNativeLinks==1&&result.Links.Count==2&&result.Unavailable.Count==0,"Source collision splits did not produce both links");
        var back=MSBB.Read(native.Write());Check(back.Parts.ConnectCollisions.Count==2&&back.Parts.ConnectCollisions.All(p=>p.MapID.SequenceEqual(new byte[]{24,0,0,0})),"Native map target did not round-trip");
        Check(back.Parts.ConnectCollisions.Select(p=>p.CollisionName).Order().SequenceEqual(new[]{"floor_a","floor_b"}),"Native collision references did not round-trip");
        var variant=new MSBB();variant.Models.Collisions.Add((MSBB.Model.Collision)model.DeepCopy());
        var va=(MSBB.Part.Collision)a.DeepCopy();va.Name="variant_floor";va.EntityID=77;variant.Parts.Collisions.Add(va);
        a.MapNameID=-30000;a.GparamConfigStruct.LightSetID=30010;
        var mirrored=MapRuntimeLinks.MirrorCollisionBindings(variant,[a]);
        Check(mirrored[0].Name=="variant_floor"&&va.EntityID==77&&va.MapNameID==-30000&&va.GparamConfigStruct.LightSetID==30010,"Variant collision identity or import metadata was lost");
        var phaseLinks=MapRuntimeLinks.Apply(variant,source,[chunks[0]],mirrored,new Dictionary<string,string>{{"m10_01_00_00","m24_00_00_00"}});
        Check(phaseLinks.Links.Single().TargetCollision=="variant_floor"&&MSBB.Read(variant.Write()).Parts.ConnectCollisions.Single().CollisionName=="variant_floor","Variant links copied foreign collision names");
        var baseTarget=MapRuntimeLinks.Apply(native,source,chunks,[a,b],new Dictionary<string,string>{{"m10_01_00_00","m24_01_00_01"}});
        Check(baseTarget.Links.All(l=>l.TargetMap=="m24_01_00_00")&&native.Parts.ConnectCollisions.All(p=>p.MapID.SequenceEqual(new byte[]{24,1,0,0})),"Story variant was emitted as a separate runtime map identity");
        var disabled=MapRuntimeLinks.Apply(native,source,chunks,[a,b],new Dictionary<string,string>());
        Check(disabled.RemovedNativeLinks==2&&native.Parts.ConnectCollisions.Count==0,"Disconnected import retains native streaming");
        link.CollisionName="camera_only";result=MapRuntimeLinks.Apply(native,source,chunks,[a,b],new Dictionary<string,string>{{"m10_01_00_00","m24_00_00_00"}});
        Check(result.Unavailable.Count==1&&result.Links.Count==0,"Non-solid collision invented a loading trigger");
        bool rejected=false;
        try {MapRuntimeLinks.Apply(native,source,chunks,[a,b],new Dictionary<string,string>{{"m10_01_00_00","m24_00_00_00"},{"m10_01_00_01","m24_02_00_00"}});}catch(InvalidDataException){rejected=true;}
        Check(rejected,"Wildcard ambiguity silently chose a destination");
        Console.WriteLine("Native map loading links passed: wildcard resolution, split collision bindings, MSB readback and stale-link removal");
    }
}
