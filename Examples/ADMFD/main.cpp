#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "inmost.h"
using namespace INMOST;

#ifndef M_PI
#define M_PI 3.141592653589
#endif

#if defined(USE_MPI)
#define BARRIER MPI_Barrier(MPI_COMM_WORLD);
#else
#define BARRIER
#endif

//shortcuts
typedef Storage::real real;
typedef Storage::integer integer;
typedef Storage::enumerator enumerator;
typedef Storage::real_array real_array;
typedef Storage::var_array var_array;

const real reg_abs = 1.0e-12; //regularize abs(x) as sqrt(x*x+reg_abs)
const real reg_div = 1.0e-15; //regularize (|x|+reg_div)/(|x|+|y|+2*reg_div) to reduce to 1/2 when |x| ~= |y| ~= 0




int main(int argc,char ** argv)
{
	Solver::Initialize(&argc,&argv,""); // Initialize the solver and MPI activity
#if defined(USE_PARTITIONER)
	Partitioner::Initialize(&argc,&argv); // Initialize the partitioner activity
#endif
	if( argc > 1 )
	{
    double ttt; // Variable used to measure timing
    bool repartition = false; // Is it required to redistribute the mesh?
		Mesh * m = new Mesh(); // Create an empty mesh
    { // Load the mesh
		  ttt = Timer();
		  m->SetCommunicator(INMOST_MPI_COMM_WORLD); // Set the MPI communicator for the mesh
      if( m->GetProcessorRank() == 0 ) std::cout << "Processors: " << m->GetProcessorsNumber() << std::endl;
		  if( m->isParallelFileFormat(argv[1]) ) //The format is 
		  {
			  m->Load(argv[1]); // Load mesh from the parallel file format
			  repartition = true; // Ask to repartition the mesh
		  }
		  else if( m->GetProcessorRank() == 0 ) m->Load(argv[1]); // Load mesh from the serial file format
		  BARRIER
		  if( m->GetProcessorRank() == 0 ) std::cout << "Load the mesh: " << Timer()-ttt << std::endl;
    }
		
		
#if defined(USE_PARTITIONER)
    if (m->GetProcessorsNumber() > 1 && !repartition) // Currently only non-distributed meshes are supported by Inner_RCM partitioner
    {
      { // Compute mesh partitioning
			  ttt = Timer();
			  Partitioner p(m); //Create Partitioning object
			  p.SetMethod(Partitioner::Inner_RCM,repartition ? Partitioner::Repartition : Partitioner::Partition); // Specify the partitioner
			  p.Evaluate(); // Compute the partitioner and store new processor ID in the mesh
			  BARRIER
			  if( m->GetProcessorRank() == 0 ) std::cout << "Evaluate: " << Timer()-ttt << std::endl;
      }

      { //Distribute the mesh
			  ttt = Timer();
			  m->Redistribute(); // Redistribute the mesh data
			  m->ReorderEmpty(CELL|FACE|EDGE|NODE); // Clean the data after reordring
			  BARRIER
			  if( m->GetProcessorRank() == 0 ) std::cout << "Redistribute: " << Timer()-ttt << std::endl;
      }
		}
#endif

    { // prepare geometrical data on the mesh
      ttt = Timer();
		  Mesh::GeomParam table;
		  table[CENTROID]    = CELL | FACE; //Compute averaged center of mass
		  table[NORMAL]      = FACE;        //Compute normals
		  table[ORIENTATION] = FACE;        //Check and fix normal orientation
		  table[MEASURE]     = CELL | FACE; //Compute volumes and areas
		  //table[BARYCENTER]  = CELL | FACE; //Compute volumetric center of mass
		  m->PrepareGeometricData(table); //Ask to precompute the data
		  BARRIER
		  if( m->GetProcessorRank() == 0 ) std::cout << "Prepare geometric data: " << Timer()-ttt << std::endl;
    }
		
    // data tags for
		Tag tag_P;  // Pressure
		Tag tag_K;  // Diffusion tensor
    Tag tag_F;  // Forcing term
    Tag tag_BC; // Boundary conditions
    Tag tag_M;  // Stiffness matrix
    Tag tag_W;  // Gradient matrix acting on harmonic points on faces and returning gradient on faces
    Tag tag_D;  // Entries for scaling matrix D
    Tag tag_LF; // Coefficients of linearly computed fluxes on faces, 2 of them per internal face
    Tag tag_MF; // Fluxes after inner product

    tag_MF = m->CreateTag("IPFLUX",DATA_VARIABLE,FACE,NONE,1);

    if( m->GetProcessorsNumber() > 1 ) //skip for one processor job
    { // Exchange ghost cells
		  ttt = Timer();
		  m->ExchangeGhost(1,FACE); // Produce layer of ghost cells
		  BARRIER
		  if( m->GetProcessorRank() == 0 ) std::cout << "Exchange ghost: " << Timer()-ttt << std::endl;
    }
    
    { //initialize data
		  if( m->HaveTag("PERM") ) // is diffusion tensor already defined on the mesh? (PERM from permeability)
        tag_K = m->GetTag("PERM"); // get the diffusion tensor
      
      if( !tag_K.isValid() || !tag_K.isDefined(CELL) ) // diffusion tensor was not initialized or was not defined on cells.
      {
        tag_K = m->CreateTag("PERM",DATA_REAL,CELL,NONE,6); // create a new tag for symmetric diffusion tensor K
        for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) ) // loop over mesh cells
        {
          Cell cell = m->CellByLocalID(q);
          real_array K = cell->RealArray(tag_K);
          // assign a symmetric positive definite tensor K
				  K[0] = 1.0; //XX
          K[1] = 0.0; //XY
          K[2] = 0.0; //XZ
          K[3] = 1.0; //YY
          K[4] = 0.0; //YZ
          K[5] = 1.0; //ZZ
        }

        m->ExchangeData(tag_K,CELL,0); //Exchange diffusion tensor
      }

      if( m->HaveTag("PRESSURE") ) //Is there a pressure on the mesh?
        tag_P = m->GetTag("PRESSURE"); //Get the pressure
      
      if( !tag_P.isValid() || !tag_P.isDefined(CELL) ) // Pressure was not initialized or was not defined on nodes
      {
        srand(1); // Randomization
        tag_P = m->CreateTag("PRESSURE",DATA_REAL,CELL|FACE,NONE,1); // Create a new tag for the pressure
        for(Mesh::iteratorElement e = m->BeginElement(CELL|FACE); e != m->EndElement(); ++e) //Loop over mesh cells
            e->Real(tag_P) = 0;//(rand()*1.0)/(RAND_MAX*1.0); // Prescribe random value in [0,1]
      }

      if( !tag_P.isDefined(FACE) )
      {
        tag_P = m->CreateTag("PRESSURE",DATA_REAL,FACE,NONE,1);
        for(Mesh::iteratorElement e = m->BeginElement(FACE); e != m->EndElement(); ++e) //Loop over mesh cells
            e->Real(tag_P) = 0;//(rand()*1.0)/(RAND_MAX*1.0); // Prescribe random value in [0,1]
      }


      
      if( m->HaveTag("BOUNDARY_CONDITION") ) //Is there boundary condition on the mesh?
      {
        tag_BC = m->GetTag("BOUNDARY_CONDITION");
        
        //initialize unknowns at boundary
      }
      m->ExchangeData(tag_P,CELL|FACE,0); //Synchronize initial solution with boundary unknowns
      //run in a loop to identify boundary pressures so that they enter as primary variables
      tag_M = m->CreateTag("INNER_PRODUCT",DATA_REAL,CELL,NONE);
      //assemble inner product matrix M acting on faces for each cell
      for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) )
      {
        Cell cell = m->CellByLocalID(q);
        real xP[3]; //center of the cell
        real nF[3]; //normal to the face
        real yF[3]; //center of the face
        real aF; //area of the face
        real vP = cell->Volume(); //volume of the cell
        cell->Centroid(xP); //obtain cell center
        ElementArray<Face> faces = cell->getFaces(); //obtain faces of the cell
        int NF = (int)faces.size(); //number of faces;
        rMatrix IP(NF,NF), N(NF,3), R(NF,3); //matrices for inner product
        rMatrix K = rMatrix::FromTensor(cell->RealArrayDF(tag_K).data(),cell->RealArrayDF(tag_K).size()); //get permeability for the cell
        //assemble matrices R and N
        //follows chapter 5.1.4
        //in the book "Mimetic Finite Difference Method for Elliptic Problems" 
        //by Lourenco et al
        for(int k = 0; k < NF; ++k)
        {
          aF = faces[k]->Area();
          faces[k]->Centroid(yF); //point on face
          faces[k]->OrientedUnitNormal(cell->self(),nF);
          // assemble R matrix, formula (5.29)
          R(k,0) = (yF[0]-xP[0])*aF;
          R(k,1) = (yF[1]-xP[1])*aF;
          R(k,2) = (yF[2]-xP[2])*aF;
          // assemble N marix, formula (5.25)
          //N(k,0) = nF[0]*aF;
          //N(k,1) = nF[1]*aF;
          //N(k,2) = nF[2]*aF;
          rMatrix nK = rMatrix::FromVector(nF,3).Transpose()*K;
          N(k,0) = nK(0,0);
          N(k,1) = nK(0,1);
          N(k,2) = nK(0,2);
        } //end of loop over faces
        // formula (5.31)
        IP = R*(R.Transpose()*N).Invert(true).first*R.Transpose(); // Consistency part
        // formula (5.32)
        IP += (rMatrix::Unit(NF) - N*(N.Transpose()*N).Invert(true).first*N.Transpose())*(2.0/(static_cast<real>(NF)*vP)*(R*K.Invert(true).first*R.Transpose()).Trace()); //Stability part
        //assert(IP.isSymmetric()); //test positive definitiness as well!
        /*
        if( !IP.isSymmetric() ) 
        {
          std::cout << "unsymmetric" << std::endl;
          IP.Print();
          std::cout << "check" << std::endl;
          (IP-IP.Transpose()).Print();
        }
        */
        real_array M = cell->RealArrayDV(tag_M); //access data structure for inner product matrix in mesh
        M.resize(NF*NF); //resize variable array
        std::copy(IP.data(),IP.data()+NF*NF,M.data()); //write down the inner product matrix
      } //end of loop over cells

      tag_W = m->CreateTag("GRAD",DATA_REAL,CELL,NONE);
      tag_D = m->CreateTag("DIAG",DATA_VARIABLE,CELL,NONE);
      //Assemble gradient matrix W on cells
      for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) )
      {
        Cell cell = m->CellByLocalID(q);
        real xP[3]; //center of the cell
        real yF[3]; //center of the face
        real nF[3]; //normal to the face
        real aF; //area of the face
        real vP = cell->Volume(); //volume of the cell
        cell->Centroid(xP);
        ElementArray<Face> faces = cell->getFaces(); //obtain faces of the cell
        int NF = (int)faces.size(); //number of faces;
        rMatrix K = rMatrix::FromTensor(cell->RealArrayDF(tag_K).data(),cell->RealArrayDF(tag_K).size()); //get permeability for the cell
        //rMatrix U,S,V;
        //K0.SVD(U,S,V);
        //for(int k = 0; k < 3; ++k) S(k,k) = sqrt(S(k,k));
        //rMatrix K = U*S*V;
        rMatrix GRAD(NF,NF), NK(NF,3), R(NF,3); //big gradient matrix, co-normals, directions
        for(int k = 0; k < NF; ++k) //loop over faces
        {
          aF = faces[k]->Area();
          faces[k]->Centroid(yF);
          faces[k]->OrientedUnitNormal(cell->self(),nF);
          // assemble matrix of directions
            R(k,0) = (yF[0]-xP[0]);//*aF;
            R(k,1) = (yF[1]-xP[1]);//*aF;
            R(k,2) = (yF[2]-xP[2]);//*aF;
          // assemble matrix of co-normals 
          rMatrix nK = rMatrix::FromVector(nF,3).Transpose()*K;
          NK(k,0) = nK(0,0);
          NK(k,1) = nK(0,1);
          NK(k,2) = nK(0,2);
        } //end of loop over faces
        GRAD = NK*(NK.Transpose()*R).Invert(true).first*NK.Transpose(); //stability part
        GRAD += (rMatrix::Unit(NF) - R*(R.Transpose()*R).Invert(true).first*R.Transpose())*(2.0/(static_cast<real>(NF)*vP)*(NK*K.Invert(true).first*NK.Transpose()).Trace());
        //GRAD += (rMatrix::Unit(NF) - R*(R.Transpose()*R).Invert().first*R.Transpose())*(2.0/(static_cast<real>(NF))*GRAD.Trace());
        real_array W = cell->RealArrayDV(tag_W); //access data structure for gradient matrix in mesh
        W.resize(NF*NF); //resize the structure
        std::copy(GRAD.data(),GRAD.data()+NF*NF,W.data()); //write down the gradient matrix
        cell->VariableArrayDV(tag_D).resize(NF); //resize scaling matrix D for the future use
      } //end of loop over cells

      if( m->HaveTag("FORCE") ) //Is there force on the mesh?
      {
        tag_F = m->GetTag("FORCE"); //initial force
        assert(tag_F.isDefined(CELL)); //assuming it was defined on cells
      } // end of force

      tag_LF = m->CreateTag("LINEAR_FLUX",DATA_VARIABLE,FACE,NONE,2);
    } //end of initialize data

   
		
    

    { //Main loop for problem solution
      Automatizator aut(m); // declare class to help manage unknowns
      Automatizator::MakeCurrent(&aut);
      dynamic_variable P(aut,aut.RegisterDynamicTag(tag_P,CELL|FACE)); //register pressure as primary unknown
      variable calc; //declare variable that helps calculating the value with variations
      aut.EnumerateDynamicTags(); //enumerate all primary variables

     
      Residual R("",aut.GetFirstIndex(),aut.GetLastIndex());
      Sparse::Vector Update  ("",aut.GetFirstIndex(),aut.GetLastIndex()); //vector for update
      
      {//Annotate matrix
        for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) )
        {
          Cell cell = m->CellByLocalID(q);
          R.GetJacobian().Annotation(P.Index(cell)) = "Cell-centered pressure value";
        }
        for( int q = 0; q < m->FaceLastLocalID(); ++q ) if( m->isValidFace(q) )
        {
          Face face = m->FaceByLocalID(q);
          if( tag_BC.isValid() && face.HaveData(tag_BC) )
            R.GetJacobian().Annotation(P.Index(face)) = "Pressure guided by boundary condition";
          else
            R.GetJacobian().Annotation(P.Index(face)) = "Interface pressure";
        }
      }

      do
      {
        R.Clear(); //clean up the residual
        for( int q = 0; q < m->FaceLastLocalID(); ++q ) if( m->isValidFace(q) )
          m->FaceByLocalID(q)->Variable(tag_MF) = 0.0;
        //First we need to evaluate the gradient at each cell for scaling matrix D
        for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) ) //loop over cells
        {
          Cell cell = m->CellByLocalID(q);
          ElementArray<Face> faces = cell->getFaces(); //obtain faces of the cell
          int NF = (int)faces.size();
          Cell cK = cell->self();
          rMatrix GRAD(cK->RealArrayDV(tag_W).data(),NF,NF); //Matrix for gradient
          vMatrix pF(NF,1); //vector of pressure differences on faces
          vMatrix FLUX(NF,1); //computed flux on faces
          vMatrix MFLUX(NF,1);
          rMatrix M(cell->RealArrayDV(tag_M).data(),NF,NF); //inner product matrix
          for(int k = 0; k < NF; ++k)
            pF(k,0) = P(faces[k]) - P(cK);
          FLUX = GRAD*pF; //fluxes on faces
          for(int k = 0; k < NF; ++k) //copy the computed flux value with variations into mesh
            faces[k]->VariableArray(tag_LF)[(faces[k]->BackCell() == cell)? 0 : 1] = FLUX(k,0);
          MFLUX = M*FLUX;
          for(int k = 0; k < NF; ++k)
            faces[k]->Variable(tag_MF) += MFLUX(k,0)*(faces[k].FaceOrientedOutside(cell) ? 1 : -1);
        } //end of loop over cells

        //Now we need to assemble and transpose nonlinear gradient matrix
        for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) ) //loop over cells
        {
          const real eps1 = 1.0e-3;
          const real eps2 = 1.0e-9;
          Cell cell = m->CellByLocalID(q);
          ElementArray<Face> faces = cell->getFaces(); //obtain faces of the cell
          int NF = (int)faces.size();
          rMatrix GRAD(cell->RealArrayDV(tag_W).data(),NF,NF); //Matrix for gradient
          rMatrix M(cell->RealArrayDV(tag_M).data(),NF,NF); //inner product matrix
          vMatrix D(NF,NF); //Matrix for diagonal
          vMatrix FLUX(NF,1); //computed flux on faces
          D.Zero();
          //assemble diagonal matrix, loop through faces and access corresponding entries
          for(int k = 0; k < NF; ++k) //loop over faces of current cell
          {
            var_array LF = faces[k]->VariableArray(tag_LF);
            variable & u = LF[(faces[k]->BackCell() == cell) ? 0 : 1]; //my flux value
            if( faces[k].Boundary() ) 
            {
              D(k,k) = 1.0; // no flux balancing on the boundary
              FLUX(k,0) = u; //restore matrix of fluxes
            }
            else
            {
              variable & v = LF[(faces[k]->BackCell() == cell) ? 1 : 0]; //neighbour flux value
              //single flux definition
              /*
              FLUX(k,0) = (soft_abs(u,eps1)+eps2)*(soft_abs(v,eps1)+eps2)/(soft_abs(u,eps1)+soft_abs(v,eps1)+2*eps2)*(soft_sign(u,eps1) + soft_sign(v,eps1)); //restore matrix of fluxes
              if( u*v > 0 )
                  D(k,k) = 2.0*(soft_abs(v,eps1)+eps2)/(soft_abs(u,eps1)+soft_abs(v,eps1)+2*eps2);
              else
                  D(k,k) = 0;
              */
              //D(k,k) = FLUX(k,0) / u;
              //dual flux definition
              //FLUX(k,0) = u*D(k,k);
              //  FLUX(k,0) = ((u*u+eps2)*v+(v*v+eps2)*u)/(u*u+v*v + 2*eps2);
              
                //D(k,k) = 1;//(v*v+eps2)/(u*u+v*v+2*eps2);
                if( u*v < 0 )
                  D(k,k) = (2*(soft_abs(v,eps1)+eps2)/(soft_abs(u,eps1)+soft_abs(v,eps1)+2*eps2));
                else
                  D(k,k) = eps2;
                D(k,k) = 1;
                FLUX(k,0) = u*D(k,k);
            }
            //FLUX(k,0) = faces[k]->Variable(tag_MF)*(faces[k].FaceOrientedOutside(cell) ? 1 : -1);
          }
          vMatrix DIV = -(D*GRAD).Transpose()*M; //cell-wise div

          /*
          std::cout << "D" << std::endl;
          D.Print();
          std::cout << "GRAD" << std::endl;
          GRAD.Print();
          std::cout << "M" << std::endl;
          M.Print();
          std::cout << "DIV" << std::endl;
          DIV.Print();
          */
          vMatrix DIVKGRAD = DIV*FLUX;

          //std::cout << "DIVKGRADp" << std::endl;
          //DIVKGRAD.Print();

          for(int k = 0; k < NF; ++k) //loop over faces of current cell
            R[P.Index(cell)] -= DIVKGRAD(k,0);
          for(int k = 0; k < NF; ++k) //loop over faces of current cell
          {
            int index = P.Index(faces[k]);
            if( tag_BC.isValid() && faces[k].HaveData(tag_BC) )
            {
              real_array BC = faces[k].RealArray(tag_BC);
              R[index] -= BC[0]*P(faces[k]) + BC[1]*DIVKGRAD(k,0) - BC[2];
            }
            else
              R[index] -= DIVKGRAD(k,0);
          }
        }

        if( tag_F.isValid() )
        {
          for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) )
          {
            Cell cell = m->CellByLocalID(q);
            if( cell->HaveData(tag_F) ) R[P.Index(cell)] += cell->Real(tag_F)*cell->Volume();
          }
        }
        
        R.GetJacobian().Save("jacobian.mtx");
        R.GetResidual().Save("residual.mtx");


        std::cout << "Nonlinear residual: " << R.Norm() << std::endl;

        Solver S(Solver::INNER_MPTILUC);
        S.SetMatrix(R.GetJacobian());
          S.SetParameterReal("drop_tolerance", 1.0e-4);
          S.SetParameterReal("reuse_tolerance", 1.0e-6);
        //std::fill(Update.Begin(),Update.End(),0.0);
        if( S.Solve(R.GetResidual(),Update) )
        {
          for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) )
          {
            Cell cell = m->CellByLocalID(q);
            cell->Real(tag_P) -= Update[P.Index(cell)];
          }
          for( int q = 0; q < m->FaceLastLocalID(); ++q ) if( m->isValidFace(q) )
          {
            Face face = m->FaceByLocalID(q);
            face->Real(tag_P) -= Update[P.Index(face)];
          }
          m->Save("iter.vtk");
        }
        else
        {
          std::cout << "Unable to solve: " << S.GetReason() << std::endl;
            break;
        }
      
      } while( R.Norm() > 1.0e-4 ); //check the residual norm
    } 
    
    if( m->HaveTag("REFERENCE_SOLUTION") )
    {
      Tag tag_E = m->CreateTag("ERRROR",DATA_REAL,CELL,NONE,1);
      Tag tag_R = m->GetTag("REFERENCE_SOLUTION");
      real C, L2, volume;
      C = L2 = volume = 0.0;
      for( int q = 0; q < m->CellLastLocalID(); ++q ) if( m->isValidCell(q) )
      {
        Cell cell = m->CellByLocalID(q);
        real err = cell->Real(tag_P) - cell->Real(tag_R);
        real vol = cell->Volume();
        if( C < fabs(err) ) C = fabs(err);
        L2 += err*err*vol;
        volume += vol;
        cell->Real(tag_E) = err;
      }
      L2 = sqrt(L2/volume);
      std::cout << "Error on cells, C-norm " << C << " L2-norm " << L2 << std::endl;
      C = L2 = volume = 0.0;
      if( tag_R.isDefined(FACE) )
      {
        tag_E = m->CreateTag("ERRROR",DATA_REAL,FACE,NONE,1);
        for( int q = 0; q < m->FaceLastLocalID(); ++q ) if( m->isValidFace(q) )
        {
          Face face = m->FaceByLocalID(q);
          real err = face->Real(tag_P) - face->Real(tag_R);
          real vol = (face->BackCell()->Volume() + (face->FrontCell().isValid() ? face->FrontCell()->Volume() : 0))*0.5;
          if( C < fabs(err) ) C = fabs(err);
          L2 += err*err*vol;
          volume += vol;
          face->Real(tag_E) = err;
        }
        L2 = sqrt(L2/volume);
        std::cout << "Error on faces, C-norm " << C << " L2-norm " << L2 << std::endl;
      }
      else std::cout << "Reference solution was not defined on faces" << std::endl;
    }

    m->Save("out.gmv");
    m->Save("out.vtk");

		delete m; //clean up the mesh
	}
	else
	{
		std::cout << argv[0] << " mesh_file" << std::endl;
	}

#if defined(USE_PARTITIONER)
	Partitioner::Finalize(); // Finalize the partitioner activity
#endif
	Solver::Finalize(); // Finalize solver and close MPI activity
	return 0;
}
