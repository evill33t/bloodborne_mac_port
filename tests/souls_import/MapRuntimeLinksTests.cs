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
        // Additional Firelink preloads the actual aqueduct group from the stair
        // base. Earlier source connectors request different Burg subgroups.
        var approachSource=new MSB1();var approachFloor=new MSB1.Part.Collision{Name="h0027B2_0000",ModelName="h0027B2",HitFilterID=8};approachSource.Parts.Collisions.Add(approachFloor);
        approachSource.Models.Collisions.Add(new(){Name="h0027B2"});approachSource.Models.Collisions.Add(new(){Name="h0030B2"});approachSource.Parts.Collisions.Add(new(){Name="aqueduct",ModelName="h0030B2",HitFilterID=8});
        var authored=new MSB1.Part.ConnectCollision{Name="h0030B2_0001",CollisionName="aqueduct",ModelName="h0030B2"};Array.Copy(wildcard,authored.MapID,4);Array.Clear(authored.DrawGroups);Array.Clear(authored.DispGroups);authored.DispGroups[2]=64;approachSource.Parts.ConnectCollisions.Add(authored);
        var approachMap=new MSBB();approachMap.Models.Collisions.Add(new(){Name="h010000"});approachMap.Models.Collisions.Add(new(){Name="h010001"});
        var stairs=new MSBB.Part.Collision{Name="stairs",ModelName="h010000",Position=new Vector3(1,2,3)};var corridor=new MSBB.Part.Collision{Name="corridor",ModelName="h010001"};approachMap.Parts.Collisions.AddRange([stairs,corridor]);
        var approachChunks=new[]{new MapCollisionBindings.Chunk(approachFloor.Name,[],[]),new MapCollisionBindings.Chunk("aqueduct",[],[])};
        var destinations=new Dictionary<string,string>{{"m10_01_00_00","m40_03_00_00"}};var sourceBytes=approachSource.Write();var floorBytes=System.Text.Json.JsonSerializer.Serialize(stairs);
        var early=MapRuntimeLinks.Apply(approachMap,approachSource,approachChunks,[stairs,corridor],destinations,"m10_02_00_00");
        Check(early.Links.Count==1&&early.ApproachPrefetch.Single().SourceFloor==approachFloor.Name,"Earlier prefetch replaced or invented source connections");
        var decoded=MSBB.Read(approachMap.Write());var preload=decoded.Parts.ConnectCollisions.Single(p=>p.CollisionName==stairs.Name);
        Check(preload.ModelName==stairs.ModelName&&preload.Position==stairs.Position&&preload.MapID.SequenceEqual(new byte[]{40,3,0,0})&&preload.DispGroups[2]==64&&preload.DispGroups.Where((_,i)=>i!=2).All(v=>v==0),"Earlier prefetch lost the actual floor, target map or authored aqueduct mask");
        Check(sourceBytes.AsSpan().SequenceEqual(approachSource.Write())&&floorBytes==System.Text.Json.JsonSerializer.Serialize(stairs),"Prefetch mutated source data or its physical floor");
        var unchanged=MapRuntimeLinks.Apply(approachMap,approachSource,approachChunks,[stairs,corridor],destinations);Check(unchanged.ApproachPrefetch.Count==0&&approachMap.Parts.ConnectCollisions.Count==1,"Replacement imports received additional-mode prefetch");
        var solo=MapRuntimeLinks.Apply(approachMap,approachSource,approachChunks,[stairs,corridor],new Dictionary<string,string>(),"m10_02_00_00");Check(solo.ApproachPrefetch.Count==0&&approachMap.Parts.ConnectCollisions.Count==0,"Standalone Firelink preloaded an unavailable Burg");
        Console.WriteLine("Native map loading links passed: wildcard resolution, split collision bindings, MSB readback and stale-link removal");
    }
}
