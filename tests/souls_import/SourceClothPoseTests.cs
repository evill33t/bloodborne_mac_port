// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

static class SourceClothPoseTests {
 static void Check(bool ok,string reason){if(!ok)throw new Exception(reason);}
 static void Reject(Action action,string reason){try{action();}catch(InvalidDataException){return;}throw new Exception(reason);}
 static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
 sealed record Residuals(int Bodies,int Constraints,int CapsulePairs,float Pivot,float Angular,float Penetration,float EnergyBefore,float EnergyAfter);
 // Decode source metadata directly rather than using the solver's Body/Joint
 // readers or residual functions. Returned transforms must satisfy the actual
 // authored capsules, pivots and angular bounds, not merely change the mesh.
 static Residuals AuditPhysics(byte[] bytes,SourceClothPose.Report result) {
  var tag=new SourceCollision.Tag(bytes);float F(int at)=>BitConverter.Int32BitsToSingle(tag.Int(at));
  Matrix4x4 Frame(int at) {var x=tag.Vector(at);var y=tag.Vector(at+16);var z=tag.Vector(at+32);var p=tag.Vector(at+48);return new(x.X,x.Y,x.Z,0,y.X,y.Y,y.Z,0,z.X,z.Y,z.Z,0,p.X,p.Y,p.Z,1);}
  string Name(SourceCollision.Tag.Item body) {var text=tag.Reference(tag.Nested(body.Type,body.At,"name","stringAndFlag").At);return Encoding.UTF8.GetString(Enumerable.Range(text.At,text.Count-1).Select(tag.Byte).ToArray());}
  var bodies=tag.Items.Where(x=>x.Count==1&&tag.Name(x.Type)=="hkpRigidBody").ToArray();var byAddress=bodies.Select((x,i)=>(x.At,i)).ToDictionary(x=>x.At,x=>x.i);var poses=result.Poses.ToDictionary(x=>x.Bone,StringComparer.Ordinal);
  Check(poses.Count==bodies.Length,"Returned cloth body identity population changed");
  var before=new Matrix4x4[bodies.Length];var after=new Matrix4x4[bodies.Length];var mass=new float[bodies.Length];var ends=new (Vector3 A,Vector3 B,float R)[bodies.Length];float energyBefore=0,energyAfter=0;
  for(int i=0;i<bodies.Length;i++) {var body=bodies[i];var pose=poses[Name(body)];before[i]=Frame(tag.Nested(body.Type,body.At,"motion","motionState","transform").At);after[i]=pose.After;mass[i]=F(tag.Nested(body.Type,body.At,"motion","inertiaAndMassInv").At+12);Check(pose.Before==before[i]&&pose.Fixed==(mass[i]==0),"Reported cloth source frame/motion differs from authored data");
   var shape=tag.Reference(tag.Nested(body.Type,body.At,"collidable","shape").At);Check(tag.Name(shape.Type)=="hkpCapsuleShape","Unexpected source contact shape");ends[i]=(tag.Vector(tag.Nested(shape.Type,shape.At,"vertexA").At),tag.Vector(tag.Nested(shape.Type,shape.At,"vertexB").At),F(tag.Nested(shape.Type,shape.At,"radius").At));
   if(mass[i]>0) {var com=tag.Vector(tag.Nested(body.Type,body.At,"motion","motionState","sweptTransform").At+64);energyBefore+=Vector3.Transform(com,before[i]).Y/mass[i]*9.80665f;energyAfter+=Vector3.Transform(com,after[i]).Y/mass[i]*9.80665f;}
  }
  float pivot=0,angular=0;int constraints=0;var connected=new HashSet<(int,int)>();
  Vector3 Axis(Matrix4x4 frame,int axis)=>Vector3.Normalize(Vector3.TransformNormal(axis switch{0=>Vector3.UnitX,1=>Vector3.UnitY,2=>Vector3.UnitZ,_=>throw new Exception("Invalid authored axis")},frame));
  foreach(var item in tag.Items.Where(x=>x.Count==1&&tag.Name(x.Type)=="hkpConstraintInstance")) {
   constraints++;int refs=tag.Nested(item.Type,item.At,"entities").At;int a=byAddress[tag.Reference(refs).At],b=byAddress[tag.Reference(refs+8).At];connected.Add((Math.Min(a,b),Math.Max(a,b)));var data=tag.Reference(tag.Nested(item.Type,item.At,"data").At);bool ragdoll=tag.Name(data.Type)=="hkpRagdollConstraintData";Check(ragdoll||tag.Name(data.Type)=="hkpBallAndSocketConstraintData","Unexpected source joint type");
   Matrix4x4 fa=ragdoll?Frame(tag.Nested(data.Type,data.At,"atoms","transforms","transformA").At):Matrix4x4.Identity,fb=ragdoll?Frame(tag.Nested(data.Type,data.At,"atoms","transforms","transformB").At):Matrix4x4.Identity;
   var pa=ragdoll?fa.Translation:tag.Vector(tag.Nested(data.Type,data.At,"atoms","pivots","translationA").At);var pb=ragdoll?fb.Translation:tag.Vector(tag.Nested(data.Type,data.At,"atoms","pivots","translationB").At);pivot=Math.Max(pivot,Vector3.Distance(Vector3.Transform(pa,after[a]),Vector3.Transform(pb,after[b])));
   if(!ragdoll)continue;var wa=fa*after[a];var wb=fb*after[b];
   foreach(string key in new[]{"coneLimit","planesLimit","twistLimit"}) {var limit=tag.Nested(data.Type,data.At,"atoms",key);if(tag.Byte(tag.Nested(limit.Type,limit.At,"isEnabled").At)==0)continue;bool twist=key=="twistLimit";int x=tag.Byte(tag.Nested(limit.Type,limit.At,twist?"twistAxis":"twistAxisInA").At),y=tag.Byte(tag.Nested(limit.Type,limit.At,twist?"refAxis":"refAxisInB").At);float min=F(tag.Nested(limit.Type,limit.At,"minAngle").At),max=F(tag.Nested(limit.Type,limit.At,"maxAngle").At),angle;
    if(twist) {var axis=Vector3.Normalize(Axis(wa,x)+Axis(wb,x));var u=Axis(wa,y);var v=Axis(wb,y);u=Vector3.Normalize(u-axis*Vector3.Dot(u,axis));v=Vector3.Normalize(v-axis*Vector3.Dot(v,axis));angle=MathF.Atan2(Vector3.Dot(Vector3.Cross(u,v),axis),Math.Clamp(Vector3.Dot(u,v),-1,1));}
    else {float dot=Math.Clamp(Vector3.Dot(Axis(wa,x),Axis(wb,y)),-1,1);angle=key=="coneLimit"?MathF.Acos(dot):MathF.Asin(dot);}
    angular=Math.Max(angular,Math.Abs(angle-Math.Clamp(angle,min,max)));
   }
  }
  // An independent convex segment-distance calculation: evaluate all four
  // boundary minima and the unconstrained interior solution, if inside.
  float SegmentDistance(Vector3 p,Vector3 q,Vector3 r,Vector3 s) {var u=q-p;var v=s-r;var w=p-r;double uu=Vector3.Dot(u,u),vv=Vector3.Dot(v,v),uv=Vector3.Dot(u,v),uw=Vector3.Dot(u,w),vw=Vector3.Dot(v,w);var candidates=new List<(double A,double B)>{(0,vv==0?0:Math.Clamp(vw/vv,0,1)),(1,vv==0?0:Math.Clamp((vw+uv)/vv,0,1)),(uu==0?0:Math.Clamp(-uw/uu,0,1),0),(uu==0?0:Math.Clamp((uv-uw)/uu,0,1),1)};double determinant=uu*vv-uv*uv;if(determinant>1e-16){double a=(uv*vw-vv*uw)/determinant,b=(uu*vw-uv*uw)/determinant;if(a>=0&&a<=1&&b>=0&&b<=1)candidates.Add((a,b));}return candidates.Min(c=>Vector3.Distance(p+u*(float)c.A,r+v*(float)c.B));}
  float penetration=0;int pairs=0;for(int a=0;a<bodies.Length;a++)for(int b=a+1;b<bodies.Length;b++) {if(mass[a]==0&&mass[b]==0||connected.Contains((a,b)))continue;pairs++;float distance=SegmentDistance(Vector3.Transform(ends[a].A,after[a]),Vector3.Transform(ends[a].B,after[a]),Vector3.Transform(ends[b].A,after[b]),Vector3.Transform(ends[b].B,after[b]));penetration=Math.Max(penetration,ends[a].R+ends[b].R-distance);}
  Check(constraints==119&&pairs==1513&&pivot<.003f&&angular<.001f&&penetration<.0001f,"Independent authored cloth constraint/contact residual exceeds budget");
  Check(Math.Abs(pivot-result.MaximumPivotError)<.00001f&&Math.Abs(angular-result.MaximumAngularError)<.0005f&&Math.Abs(penetration-result.MaximumPenetration)<.00001f&&Math.Abs(energyBefore-result.GravityEnergyBefore)<.001f&&Math.Abs(energyAfter-result.GravityEnergyAfter)<.001f,"Reported source cloth convergence differs from independent geometry/physics evaluation");
  return new(bodies.Length,constraints,pairs,pivot,angular,penetration,energyBefore,energyAfter);
 }
 public static void Main(string[] args) {
  var path=Path.Combine(args[0],"chr/c2560.chrbnd.dcx");var original=File.ReadAllBytes(path);var hash=SHA256.HashData(original);var bnd=BND3.Read(original);var model=bnd.Files.Single(f=>Leaf(f.Name)=="c2560.flver").Bytes.ToArray();var physics=bnd.Files.Single(f=>Leaf(f.Name)=="c2560_c.hkx").Bytes.ToArray();var modelHash=SHA256.HashData(model);var physicsHash=SHA256.HashData(physics);
  var source=FLVER2.Read(model);var watch=System.Diagnostics.Stopwatch.StartNew();var result=SourceClothPose.Bake(model,physics);var back=FLVER2.Read(result.Model);var report=result.Report;
  Check(report.Bodies==57&&report.DynamicBodies==51&&report.Constraints==119&&report.ContactPairs==1513,"Actual source cloth physics inventory changed");
  Check(report.SourcePhysicsSHA256==Convert.ToHexStringLower(physicsHash),"Source cloth physics provenance omitted");
  Check(report.MaximumPivotError<.003f&&report.MaximumAngularError<.001f&&report.MaximumPenetration<.0001f&&report.GravityEnergyAfter<report.GravityEnergyBefore-15,"Source gravity/constraint residuals outside declared budget");
  var independent=AuditPhysics(physics,report);
  Check(report.Poses.Where(p=>p.Fixed).All(p=>p.Before==p.After),"Kinematic source body anchors moved");
  Check(report.Poses.All(p=>p.After.M14==0&&p.After.M24==0&&p.After.M34==0&&p.After.M44==1),"hkTransform padding was treated as affine data");
  Check(report.ChangedVertices==314,"Actual authored cloth deformation population changed");
  Check(report.RigidCapeAnchor=="Spine1"&&report.CapeFixedAnchors.SequenceEqual(new[]{"L UpperArm","R UpperArm","Spine1"})&&report.CapeMeshes.SequenceEqual(new[]{3,4})&&result.Anchors.Keys.Order().SequenceEqual(report.CapeMeshes)&&result.Anchors.Values.All(n=>n==report.RigidCapeAnchor),"Rigid cape binding/provenance changed");
  var capes=Enumerable.Range(0,source.Meshes.Count).Where(i=>source.Materials[source.Meshes[i].MaterialIndex].Name=="#07#").ToArray();Check(capes.Length==2&&capes.Sum(i=>source.Meshes[i].Vertices.Count)==337,"Source cape geometry fixture changed");
  float OldMaxZ()=>capes.SelectMany(i=>source.Meshes[i].Vertices).Max(v=>v.Position.Z);float MaxZ()=>capes.SelectMany(i=>back.Meshes[i].Vertices).Max(v=>v.Position.Z);
  Check(OldMaxZ()-MaxZ()>.55f&&capes.SelectMany(i=>back.Meshes[i].Vertices).Min(v=>v.Position.Y)<.42f,"Cape did not settle from its authored extended bind pose");
  for(int m=0;m<back.Meshes.Count;m++) {
   Check(back.Meshes[m].Vertices.Count==source.Meshes[m].Vertices.Count,"Source cloth mesh lost vertices");
   for(int v=0;v<back.Meshes[m].Vertices.Count;v++) {var a=source.Meshes[m].Vertices[v];var b=back.Meshes[m].Vertices[v];
    Check(float.IsFinite(b.Position.LengthSquared())&&float.IsFinite(b.Normal.LengthSquared())&&Math.Abs(b.Normal.Length()-1)<.01f,"Invalid deformed source vertex/normal");
    Check(b.Tangents.All(t=>float.IsFinite(t.LengthSquared())),"Nonfinite cloth tangent");
    b.Position=a.Position;b.Normal=a.Normal;b.Tangents=a.Tangents.ToList();
   }
   back.Meshes[m].BoundingBox=source.Meshes[m].BoundingBox;
  }
  back.Header.BoundingBoxMin=source.Header.BoundingBoxMin;back.Header.BoundingBoxMax=source.Header.BoundingBoxMax;
  Check(back.Write().AsSpan().SequenceEqual(source.Write()),"Cloth bake changed source nodes, weights, UVs, colors, materials, faces or LODs");
  // W padding is not part of hkTransform. Replace it with NaNs independently
  // in all authored rigid body and constraint frames; the bake must be equal.
  var padding=physics.ToArray();var tag=new SourceCollision.Tag(physics);
  void Pad(int at){foreach(int offset in new[]{12,28,44,60})BitConverter.GetBytes(float.NaN).CopyTo(padding,at+offset);}
  foreach(var body in tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpRigidBody"))Pad(tag.Nested(body.Type,body.At,"motion","motionState","transform").At);
  foreach(var data in tag.Items.Where(i=>i.Count==1&&tag.Name(i.Type)=="hkpRagdollConstraintData")){Pad(tag.Nested(data.Type,data.At,"atoms","transforms","transformA").At);Pad(tag.Nested(data.Type,data.At,"atoms","transforms","transformB").At);}
  var padded=SourceClothPose.Bake(model,padding);Check(padded.Model.AsSpan().SequenceEqual(result.Model),"Unused source hkTransform W padding changed gravity result");
  Check(padded.Report.Poses.SequenceEqual(report.Poses),"Gravity solver is not deterministic under equivalent source physics");
  var bad=physics.ToArray();var first=tag.Items.First(i=>i.Count==1&&tag.Name(i.Type)=="hkpRigidBody");bad[tag.Nested(first.Type,first.At,"motion","type").At]=7;Reject(()=>SourceClothPose.Bake(model,bad),"Unsupported source motion was accepted");
  bad=physics.ToArray();BitConverter.GetBytes(1u).CopyTo(bad,tag.Nested(first.Type,first.At,"collidable","broadPhaseHandle","collisionFilterInfo").At);Reject(()=>SourceClothPose.Bake(model,bad),"Unsupported source collision filtering was accepted");
  bad=physics.ToArray();var firstShape=tag.Reference(tag.Nested(first.Type,first.At,"collidable","shape").At);BitConverter.GetBytes(float.NaN).CopyTo(bad,tag.Nested(firstShape.Type,firstShape.At,"vertexA").At);Reject(()=>SourceClothPose.Bake(model,bad),"Nonfinite source capsule endpoint was accepted");
  var badModel=FLVER2.Read(model);badModel.Meshes[3].Vertices[0].BoneWeights[0]=float.NaN;Reject(()=>SourceClothPose.Bake(badModel.Write(),physics),"Nonfinite source skin weights were accepted");
  Reject(()=>SourceClothPose.Bake(model,physics[..100]),"Truncated source cloth physics was accepted");
  Check(SourceClothPose.ReadPhysics(_=>throw new Exception("Unexpected source read"),"DS2","c2560")==null&&SourceClothPose.ReadPhysics(_=>throw new Exception("Unexpected source read"),"DS1R","c2550")==null,"Unconverted source profile/model was changed");
  Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash)&&SHA256.HashData(model).AsSpan().SequenceEqual(modelHash)&&SHA256.HashData(physics).AsSpan().SequenceEqual(physicsHash),"Original source asset/input bytes changed");
  Directory.CreateDirectory(args[1]);File.WriteAllText(Path.Combine(args[1],"cloth-regression.json"),JsonSerializer.Serialize(new{status="PASS",sourcePath=path,sourceSHA256=Convert.ToHexStringLower(hash),elapsedSeconds=watch.Elapsed.TotalSeconds,independent,report},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
  Console.WriteLine($"PASS source cloth: all337 cape vertices/geometry retained;314 source vertices settled;57capsules/119constraints; pivot {report.MaximumPivotError*1000:F3}mm, angular {report.MaximumAngularError:F6}rad, penetration0; equivalent-W-padding/readback/invalid-input/original-hash guards");
 }
}
