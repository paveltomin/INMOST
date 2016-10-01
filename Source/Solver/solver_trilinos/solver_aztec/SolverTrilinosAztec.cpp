#include "SolverTrilinosAztec.h"

namespace INMOST {

    SolverTrilinosAztec::SolverTrilinosAztec() {

    }

    SolverTrilinosAztec::SolverTrilinosAztec(const SolverInterface *other) {
        //You should not really want to copy solver's information
        throw INMOST::SolverUnsupportedOperation;
    }

    bool SolverTrilinosAztec::Solve(INMOST::Sparse::Vector &RHS, INMOST::Sparse::Vector &SOL) {
        std::string name = Epetra_problem->first;
        Epetra_LinearProblem *Epetra_linear_problem = &Epetra_problem->second;
        Epetra_Vector VectorRHS(View, matrix->Map(), &*RHS.Begin());
        Epetra_Vector VectorSOL(View, matrix->Map(), &*SOL.Begin());
        Epetra_linear_problem->SetRHS(&VectorRHS);
        Epetra_linear_problem->SetLHS(&VectorSOL);

        bool have_params = parameters_file != "";
        const Teuchos::RCP<Teuchos::ParameterList> top_level_params = Teuchos::createParameterList();
        Teuchos::ParameterList local_list;
        if (have_params) {
            Teuchos::updateParametersFromXmlFileAndBroadcast(parameters_file, top_level_params.ptr(),
                                                             Teuchos::MpiComm<int>(Teuchos::opaqueWrapper(
                                                                     RHS.GetCommunicator())));
            if (!top_level_params->isSublist(name))
                have_params = false;
            else {
                local_list = top_level_params->sublist(name);
            }
        }

        AztecOO AztecSolver(*Epetra_linear_problem);

        if (have_params && local_list.isSublist("AztecOO")) {
            Teuchos::ParameterList AztecOOParams = local_list.sublist("AztecOO");
            if (AztecOOParams.isParameter("Max Iterations")) {
                parameters.SetParameterEnum("maximum_iterations", AztecOOParams.get<int>("Max Iterations"));
            }
            if (AztecOOParams.isParameter("Tolerance")) {
                parameters.SetParameterReal("relative_tolerance", AztecOOParams.get<double>("Tolerance"));
            }
            if (AztecOOParams.isSublist("AztecOO Settings")) {
                AztecSolver.SetParameters(AztecOOParams.sublist("AztecOO Settings"));
            }
        } else {
            AztecSolver.SetAztecOption(AZ_diagnostics, AZ_none);
            AztecSolver.SetAztecOption(AZ_output, AZ_none);
            AztecSolver.SetAztecOption(AZ_solver, AZ_bicgstab);
            AztecSolver.SetAztecOption(AZ_overlap, parameters.GetParameterEnum("additive_schwartz_overlap"));
        }

        if (!have_params) {
            AztecSolver.SetAztecParam(AZ_drop, parameters.GetParameterReal("drop_tolerance"));
            AztecSolver.SetAztecParam(AZ_ilut_fill, parameters.GetParameterReal("fill_level"));
        }

        AztecSolver.Iterate(parameters.GetParameterEnum("maximum_iterations"), parameters.GetParameterReal("relative_tolerance"));
        const double *stats = AztecSolver.GetAztecStatus();
        bool success = true;
        std::string reason = "";
        TrilinosCheckStatus(static_cast<int>(stats[AZ_why]), success, reason);
        lastIterations = AztecSolver.NumIters();
        lastResidual = AztecSolver.TrueResidual();
        returnReason = reason;
        return success;
    }

    const std::string SolverTrilinosAztec::SolverName() const {
        return "trilinos_aztec";
    }

    SolverTrilinosAztec::~SolverTrilinosAztec() {
        this->Clear();
    }

}