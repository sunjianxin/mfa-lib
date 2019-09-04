//--------------------------------------------------------------
// decoder object
//
// Tom Peterka
// Argonne National Laboratory
// tpeterka@mcs.anl.gov
//--------------------------------------------------------------

#ifndef _DECODE_HPP
#define _DECODE_HPP

#include    <mfa/data_model.hpp>
#include    <mfa/mfa.hpp>

#include    <Eigen/Dense>

typedef Eigen::MatrixXi MatrixXi;

namespace mfa
{
    template <typename T>                           // float or double
    struct DecodeInfo
    {
        vector<MatrixX<T>>  N;                              // basis functions in each dim.
        vector<VectorX<T>>  temp;                           // temporary point in each dim.
        vector<int>         span;                           // current knot span in each dim.
        vector<int>         n;                              // number of control point spans in each dim
        vector<int>         iter;                           // iteration number in each dim.
        VectorX<T>          ctrl_pt;                        // one control point
        int                 ctrl_idx;                       // control point linear ordering index
        VectorX<T>          temp_denom;                     // temporary rational NURBS denominator in each dim
        vector<MatrixX<T>>  ders;                           // derivatives in each dim.

        DecodeInfo(const MFA_Data<T>&   mfa,                // current mfa
                   const VectorXi&      derivs)             // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                            // pass size-0 vector if unused
        {
            N.resize(mfa.p.size());
            temp.resize(mfa.p.size());
            span.resize(mfa.p.size());
            n.resize(mfa.p.size());
            iter.resize(mfa.p.size());
            ctrl_pt.resize(mfa.tmesh.tensor_prods[0].ctrl_pts.cols());
            temp_denom = VectorX<T>::Zero(mfa.p.size());
            ders.resize(mfa.p.size());

            for (size_t i = 0; i < mfa.dom_dim; i++)
            {
                temp[i]    = VectorX<T>::Zero(mfa.tmesh.tensor_prods[0].ctrl_pts.cols());
                // TODO: hard-coded for one tensor product
                N[i]       = MatrixX<T>::Zero(1, mfa.tmesh.tensor_prods[0].nctrl_pts(i));
                if (derivs.size() && derivs(i))
                    // TODO: hard-coded for one tensor product
                    ders[i] = MatrixX<T>::Zero(derivs(i) + 1, mfa.tmesh.tensor_prods[0].nctrl_pts(i));
            }
        }

        // reset decode info
        // version for recomputing basis functions
        void Reset(const MFA_Data<T>&   mfa,                // current mfa
                   const VectorXi&      derivs)             // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                            // pass size-0 vector if unused
        {
            temp_denom.setZero();
            for (auto i = 0; i < mfa.dom_dim; i++)
            {
                temp[i].setZero();
                iter[i] = 0;
                N[i].setZero();
                if (derivs.size() && derivs(i))
                    ders[i].setZero();
            }
        }

        // reset decode info
        // version for saved basis functions
        void Reset_saved_basis(const MFA_Data<T>&   mfa)    // current mfa
        {
            temp_denom.setZero();
            for (auto i = 0; i < mfa.dom_dim; i++)
            {
                temp[i].setZero();
                iter[i] = 0;
            }
        }
    };

    template <typename T>                               // float or double
    class Decoder
    {
    private:

        int             tot_iters;                      // total iterations in flattened decoding of all dimensions
        MatrixXi        ct;                             // coordinates of first control point of curve for given iteration
                                                        // of decoding loop, relative to start of box of
                                                        // control points
        vector<size_t>  cs;                             // control point stride (only in decoder, not mfa)
        int             verbose;                        // output level
        MFA_Data<T>&    mfa;                            // the mfa object

    public:

        Decoder(
                MFA_Data<T>& mfa_,                      // MFA data model
                int          verbose_)                  // output level
            : mfa(mfa_), verbose(verbose_)
        {
            // ensure that encoding was already done
            if (!mfa.p.size()                               ||
                !mfa.tmesh.all_knots.size()                 ||
                !mfa.tmesh.tensor_prods.size()              ||
                !mfa.tmesh.tensor_prods[0].nctrl_pts.size() ||
                !mfa.tmesh.tensor_prods[0].ctrl_pts.size())
            {
                fprintf(stderr, "Decoder() error: Attempting to decode before encoding.\n");
                exit(0);
            }

            // initialize decoding data structures
            // TODO: hard-coded for first tensor product only
            // needs to be expanded for multiple tensor products, maybe moved into the tensor product
            cs.resize(mfa.p.size(), 1);
            tot_iters = 1;                              // total number of iterations in the flattened decoding loop
            for (size_t i = 0; i < mfa.p.size(); i++)   // for all dims
            {
                tot_iters  *= (mfa.p(i) + 1);
                if (i > 0)
                    cs[i] = cs[i - 1] * mfa.tmesh.tensor_prods[0].nctrl_pts[i - 1];
            }
            ct.resize(tot_iters, mfa.p.size());

            // compute coordinates of first control point of curve corresponding to this iteration
            // these are relative to start of the box of control points located at co
            for (int i = 0; i < tot_iters; i++)      // 1-d flattening all n-d nested loop computations
            {
                int div = tot_iters;
                int i_temp = i;
                for (int j = mfa.p.size() - 1; j >= 0; j--)
                {
                    div      /= (mfa.p(j) + 1);
                    ct(i, j) =  i_temp / div;
                    i_temp   -= (ct(i, j) * div);
                }
            }
        }

        ~Decoder() {}

        // computes approximated points from a given set of domain points and an n-d NURBS volume
        // P&T eq. 9.77, p. 424
        // assumes all vectors have been correctly resized by the caller
        void DecodeDomain(
                MatrixX<T>& approx,                 // decoded output points (1st dim changes fastest)
                int         min_dim,                // first dimension to decode
                int         max_dim)                // last dimension to decode
        {
            VectorXi no_ders;                       // size 0 means no derivatives
            Decode(approx, min_dim, max_dim, no_ders);
        }

        // computes approximated points from a given set of domain points and an n-d NURBS volume
        // P&T eq. 9.77, p. 424
        // assumes all vectors have been correctly resized by the caller
        void DecodeDomain(
                MatrixX<T>& approx,                 // decoded output points (1st dim changes fastest)
                int         min_dim,                // first dimension to decode
                int         max_dim,                // last dimension to decode
                VectorXi&   derivs)                 // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                    // pass size-0 vector if unused
        {
            VectorXi iter = VectorXi::Zero(mfa.dom_dim);                    // parameter index (iteration count, ie, ijk) in current dim.
            int last = mfa.tmesh.tensor_prods[0].ctrl_pts.cols() - 1;       // last coordinate of control point

#ifndef MFA_NO_TBB                                  // TBB version, faster (~3X) than serial

            // thread-local DecodeInfo
            // ref: https://www.threadingbuildingblocks.org/tutorial-intel-tbb-thread-local-storage
            enumerable_thread_specific<DecodeInfo<T>> thread_decode_info(mfa, derivs);

            parallel_for (size_t(0), (size_t)approx.rows(), [&] (size_t i)
            {
                // convert linear idx to multidim. i,j,k... indices in each domain dimension
                VectorXi ijk(mfa.dom_dim);
                mfa.idx2ijk(i, ijk);

                // compute parameters for the vertices of the cell
                VectorX<T> param(mfa.dom_dim);
                for (int j = 0; j < mfa.dom_dim; j++)
                    param(j) = mfa.params[j][ijk(j)];

                // compute approximated point for this parameter vector
                VectorX<T> cpt(last + 1);               // evaluated point

#ifndef TMESH   // original version for one tensor product

                // using old VolPt
//                 VolPt(param, cpt, thread_decode_info.local(), mfa.tmesh.tensor_prods[0], derivs);  // faster improved VolPt
//                 if (i == 0)
//                     fprintf(stderr, "Using VolPt\n");

                // using VolPt for saved basis
                if (i == 0)
                    fprintf(stderr, "Using VolPt_saved_basis\n");
                VolPt_saved_basis(ijk, param, cpt, thread_decode_info.local(), mfa.tmesh.tensor_prods[0]);

#else           // tmesh version

                // hard-coded for first tensor product
                if (i == 0)
                    fprintf(stderr, "Using VolPt_tmesh\n");
                VolPt_tmesh(param, mfa.tmesh.tensor_prods[0].nctrl_pts, mfa.tmesh.tensor_prods[0].ctrl_pts,
                        mfa.tmesh.tensor_prods[0].weights, cpt);

#endif

                approx.block(i, min_dim, 1, max_dim - min_dim + 1) = cpt.transpose();
            });
            if (verbose)
                fprintf(stderr, "100 %% decoded\n");

#else                                               // serial version

            DecodeInfo<T> decode_info(mfa, derivs); // reusable decode point info for calling VolPt repeatedly

            VectorX<T> cpt(last + 1);               // evaluated point
            VectorX<T> param(mfa.p.size());         // parameters for one point

            for (size_t i = 0; i < approx.rows(); i++)
            {
                // extract parameter vector for one input point of all params
                for (size_t j = 0; j < mfa.dom_dim; j++)
                    param(j) = mfa.params[j][iter(j)];

                // compute approximated point for this parameter vector

#ifndef TMESH   // original version for one tensor product

                // using old VolPt
//                 VolPt(param, cpt, decode_info, mfa.tmesh.tensor_prods[0], derivs);
//                 if (i == 0)
//                     fprintf(stderr, "Using VolPt\n");

                // using VolPt for saved basis
                if (i == 0)
                    fprintf(stderr, "Using VolPt_saved_basis\n");
                VolPt_saved_basis(iter, param, cpt, decode_info, mfa.tmesh.tensor_prods[0]);

#else           // tmesh version

                // hard-coded for first tensor product
                if (i == 0)
                    fprintf(stderr, "Using VolPt_tmesh\n");
                VolPt_tmesh(param, mfa.tmesh.tensor_prods[0].nctrl_pts, mfa.tmesh.tensor_prods[0].ctrl_pts,
                        mfa.tmesh.tensor_prods[0].weights, cpt);

#endif

                // update the indices in the linearized vector of all params for next input point
                for (size_t j = 0; j < mfa.dom_dim; j++)
                {
                    if (iter(j) < mfa.ndom_pts(j) - 1)
                    {
                        iter(j)++;
                        break;
                    }
                    else
                        iter(j) = 0;
                }

                approx.block(i, min_dim, 1, max_dim - min_dim + 1) = cpt.transpose();

                // print progress
                if (verbose)
                    if (i > 0 && approx.rows() >= 100 && i % (approx.rows() / 100) == 0)
                        fprintf(stderr, "\r%.0f %% decoded", (T)i / (T)(approx.rows()) * 100);
            }

#endif

        }

        // decode a point in the t-mesh for a given set of control points
        // TODO: unoptimized; creates new DecodeInfo each time it is called
        void VolPt_tmesh(const VectorX<T>&      param,      // parameters of point to decode
                         const VectorXi&        nctrl_pts,  // number of control points
                         MatrixX<T>&            ctrl_pts,   // control points
                         VectorX<T>&            weights,    // control point weights
                         VectorX<T>&            out_pt)     // (output) point, allocated by caller
        {
            // compute range of anchor points for a given point to decode
            vector<vector<size_t>> anchors(mfa.dom_dim);                        // anchors affecting the decoding point
            mfa.tmesh.anchors(param, anchors);

            // debug: print global knots
//             for (auto i = 0; i < mfa.dom_dim; i++)
//             {
//                 fprintf(stderr, "dim %d global knots = [", i);
//                 for (auto j = 0; j < mfa.tmesh.all_knots[i].size(); j++)
//                     fprintf(stderr, "%lf ", mfa.tmesh.all_knots[i][j]);
//                 fprintf(stderr, "]\n");
//             }

            // debug: print decoding point
//             fprintf(stderr, "for decoding point = [ ");
//             for (auto i = 0; i < mfa.dom_dim; i++)
//                 fprintf(stderr, "%.3lf ", param[i]);
//             fprintf(stderr, "],\n");

            // debug: print anchors
//             for (auto i = 0; i < mfa.dom_dim; i++)
//             {
//                 fprintf(stderr, "dim %d anchors = [", i);
//                 for (auto j = 0; j < anchors[i].size(); j++)
//                     fprintf(stderr, "%ld ", anchors[i][j]);
//                 fprintf(stderr, "]\n");
//             }
//             fprintf(stderr, "\n--------------------------\n\n");

            // TODO: insert any missing knots and control points

            // compute basis functions in each dimension
            vector<MatrixX<T>> N(mfa.dom_dim);                          // basis functions in each dimension
            vector<vector<KnotIdx>> local_knot_idxs(mfa.dom_dim);       // local knot indices
            vector<size_t> center(mfa.dom_dim);                         // center anchor from anchors, offset by (p+1)/2
            for (auto i = 0; i < mfa.dom_dim; i++)
            {
                N[i] = MatrixX<T>::Zero(1, nctrl_pts(i));
                local_knot_idxs[i].resize(mfa.p(i) + 2);                // local knot vector for current dim in index space
                vector<T> local_knots(mfa.p(i) + 2);                    // local knot vector for current dim in parameter space
                size_t local_ctrl_idx = 0;                              // index into local control points

                for (auto k = 0; k < anchors[i].size(); k++)            // p + 1 anchors
                {
                    // anchor[j] = kth anchor in each dimension j
                    for (auto j = 0; j < mfa.dom_dim; j++)
                        center[j] = anchors[j][k] + (mfa.p(j) + 1) / 2; // center is offset from anchor by (p + 1) / 2
                    mfa.tmesh.local_knot_vector(center, local_knot_idxs);
                    for (auto n = 0; n < local_knot_idxs[i].size(); n++)
                        local_knots[n] = mfa.tmesh.all_knots[i][local_knot_idxs[i][n]];

                    // debug
//                     if (anchors[i][k] >= N[i].cols())
//                         fprintf(stderr, "Error: trying to access column %ld but N only has %ld columns\n", anchors[i][k] - 1, N[i].cols());

                    // writing basis function into column corresponding to anchor position makes it multiply correct control point
                    N[i](0, anchors[i][k]) = mfa.OneBasisFun(i, param(i), local_knots);

                    // debug: print local knot idxs
//                     fprintf(stderr, "local knot idxs = [");
//                     for (auto j = 0; j < local_knot_idxs[i].size(); j++)
//                         fprintf(stderr, "%ld ", local_knot_idxs[i][j]);
//                     fprintf(stderr, "]\n");
                }
            }

            // normalize sum of basis functions
            for (auto i = 0; i < mfa.dom_dim; i++)
            {
                N[i].row(0) /= N[i].row(0).sum();
                // debug
//                 cerr << "N[" << i << "]: " << N[i].row(0) << endl;
            }

            // decode the point
            VectorXi unused;                        // not using derivatives
            // TODO: hard-coded for first tensor product
            VolPt(param, out_pt, mfa.tmesh.tensor_prods[0], unused, N, nctrl_pts, ctrl_pts, weights);

            // debug
//             cerr << "out_pt: " << out_pt.transpose() << "\n" << endl;
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // slower version for single points
        void VolPt(
                VectorX<T>&         param,      // parameter value in each dim. of desired point
                VectorX<T>&         out_pt,     // (output) point, allocated by caller
                TensorProduct<T>&   tensor)     // tensor product to use for decoding
        {
            VectorXi no_ders;                   // size 0 vector means no derivatives
            VolPt(param, out_pt, tensor, no_ders);
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // faster version for multiple points, reuses decode info
        void VolPt(
                VectorX<T>&         param,      // parameter value in each dim. of desired point
                VectorX<T>&         out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&      di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                TensorProduct<T>&   tensor)     // tensor product to use for decoding
        {
            VectorXi no_ders;                   // size 0 vector means no derivatives
            VolPt(param, out_pt, di, tensor, no_ders);
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // slower version for single points
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt(
                VectorX<T>&         param,      // parameter value in each dim. of desired point
                VectorX<T>&         out_pt,     // (output) point, allocated by caller
                TensorProduct<T>&   tensor,     // tensor product to use for decoding
                VectorXi&           derivs)     // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                // pass size-0 vector if unused
        {
            int last = mfa.tmesh.tensor_prods[0].ctrl_pts.cols() - 1;     // last coordinate of control point
            if (derivs.size())                  // extra check for derivatives, won't slow down normal point evaluation
            {
                if (derivs.size() != mfa.p.size())
                {
                    fprintf(stderr, "Error: size of derivatives vector is not the same as the number of domain dimensions\n");
                    exit(0);
                }
                for (auto i = 0; i < mfa.p.size(); i++)
                    if (derivs(i) > mfa.p(i))
                        fprintf(stderr, "Warning: In dimension %d, trying to take derivative %d of an MFA with degree %d will result in 0. This may not be what you want",
                                i, derivs(i), mfa.p(i));
            }

            // init
            vector <MatrixX<T>> N(mfa.p.size());              // basis functions in each dim.
            vector<VectorX<T>>  temp(mfa.p.size());           // temporary point in each dim.
            vector<int>         span(mfa.p.size());           // span in each dim.
            vector<int>         n(mfa.p.size());              // number of control point spans in each dim
            vector<int>         iter(mfa.p.size());           // iteration number in each dim.
            VectorX<T>          ctrl_pt(last + 1);            // one control point
            int                 ctrl_idx;                     // control point linear ordering index
            VectorX<T>          temp_denom = VectorX<T>::Zero(mfa.p.size());     // temporary rational NURBS denominator in each dim

            // basis funs
            for (size_t i = 0; i < mfa.dom_dim; i++)       // for all dims
            {
                temp[i]    = VectorX<T>::Zero(last + 1);
                iter[i]    = 0;
                span[i]    = mfa.FindSpan(i, param(i), tensor);
                N[i]       = MatrixX<T>::Zero(1, tensor.nctrl_pts(i));
                if (derivs.size() && derivs(i))
                {
#ifndef TMESH       // original version for one tensor product
                    MatrixX<T> Ders = MatrixX<T>::Zero(derivs(i) + 1, tensor.nctrl_pts(i));
                    mfa.DerBasisFuns(i, param(i), span[i], derivs(i), Ders);
                    N[i].row(0) = Ders.row(derivs(i));
#endif
                }
                else
                {
#ifndef TMESH       // original version for one tensor product
                    mfa.OrigBasisFuns(i, param(i), span[i], N[i], 0);
#else               // tmesh version
                    mfa.BasisFuns(i, param(i), span[i], N[i], 0);
#endif
                }
            }

            // linear index of first control point
            ctrl_idx = 0;
            for (int j = 0; j < mfa.p.size(); j++)
                ctrl_idx += (span[j] - mfa.p(j) + ct(0, j)) * cs[j];
            size_t start_ctrl_idx = ctrl_idx;

            for (int i = 0; i < tot_iters; i++)             // 1-d flattening all n-d nested loop computations
            {
                // always compute the point in the first dimension
                ctrl_pt = tensor.ctrl_pts.row(ctrl_idx);
                T w     = tensor.weights(ctrl_idx);

#ifdef WEIGH_ALL_DIMS                               // weigh all dimensions
                temp[0] += (N[0])(0, iter[0] + span[0] - mfa.p(0)) * ctrl_pt * w;
#else                                               // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (temp[0])(j) += (N[0])(0, iter[0] + span[0] - mfa.p(0)) * ctrl_pt(j);
                (temp[0])(last) += (N[0])(0, iter[0] + span[0] - mfa.p(0)) * ctrl_pt(last) * w;
#endif

                temp_denom(0) += w * N[0](0, iter[0] + span[0] - mfa.p(0));
                iter[0]++;

                // for all dimensions except last, check if span is finished
                ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa.p.size(); k++)
                {
                    if (i < tot_iters - 1)
                        ctrl_idx += ct(i + 1, k) * cs[k];        // ctrl_idx for the next iteration (i+1)
                    if (k < mfa.p.size() - 1 && iter[k] - 1 == mfa.p(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        temp[k + 1]        += (N[k + 1])(0, iter[k + 1] + span[k + 1] - mfa.p(k + 1)) * temp[k];
                        temp_denom(k + 1)  += temp_denom(k) * N[k + 1](0, iter[k + 1] + span[k + 1] - mfa.p(k + 1));
                        temp_denom(k)       = 0.0;
                        temp[k]             = VectorX<T>::Zero(last + 1);
                        iter[k]             = 0;
                        iter[k + 1]++;
                    }
                }
            }

            T denom;                                // rational denominator
            if (derivs.size() && derivs.sum())
                denom = 1.0;                        // TODO: weights for derivatives not implemented yet
            else
                denom = temp_denom(mfa.p.size() - 1);

#ifdef WEIGH_ALL_DIMS                           // weigh all dimensions
            out_pt = temp[mfa.p.size() - 1] / denom;
#else                                           // weigh only range dimension
            out_pt   = temp[mfa.p.size() - 1];
            out_pt(last) /= denom;
#endif

        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // slower version for single points
        // custom basis and control points provided as arguments
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt(
                const VectorX<T>&   param,      // parameter value in each dim. of desired point
                VectorX<T>&         out_pt,     // (output) point, allocated by caller
                TensorProduct<T>&   tensor,     // tensor product to use for decoding
                VectorXi&           derivs,     // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                // pass size-0 vector if unused
                vector<MatrixX<T>>& N,          // basis functions in each dimension
                const VectorXi&     nctrl_pts,  // number of control points
                MatrixX<T>&         ctrl_pts,   // control points
                VectorX<T>&         weights)    // weights
        {
            int last = ctrl_pts.cols() - 1;     // last coordinate of control point
            if (derivs.size())                  // extra check for derivatives, won't slow down normal point evaluation
            {
                if (derivs.size() != mfa.p.size())
                {
                    fprintf(stderr, "Error: size of derivatives vector is not the same as the number of domain dimensions\n");
                    exit(0);
                }
                for (auto i = 0; i < mfa.p.size(); i++)
                    if (derivs(i) > mfa.p(i))
                        fprintf(stderr, "Warning: In dimension %d, trying to take derivative %d of an MFA with degree %d will result in 0. This may not be what you want",
                                i, derivs(i), mfa.p(i));
            }

            // init
            vector<VectorX<T>>  temp(mfa.p.size());           // temporary point in each dim.
            vector<int>         span(mfa.p.size());           // span in each dim.
            vector<int>         n(mfa.p.size());              // number of control point spans in each dim
            vector<int>         iter(mfa.p.size());           // iteration number in each dim.
            VectorX<T>          ctrl_pt(last + 1);            // one control point
            int                 ctrl_idx;                     // control point linear ordering index
            VectorX<T>          temp_denom = VectorX<T>::Zero(mfa.p.size());     // temporary rational NURBS denominator in each dim

            // basis funs
            for (size_t i = 0; i < mfa.p.size(); i++)       // for all dims
            {
                temp[i]    = VectorX<T>::Zero(last + 1);
                iter[i]    = 0;
                span[i]    = mfa.FindSpan(i, param(i), tensor);
                if (derivs.size() && derivs(i))
                {
#ifndef TMESH       // original version for one tensor product
                    MatrixX<T> Ders = MatrixX<T>::Zero(derivs(i) + 1, nctrl_pts(i));
                    mfa.DerBasisFuns(i, param(i), span[i], derivs(i), Ders);
                    N[i].row(0) = Ders.row(derivs(i));
#endif
                }
            }

            // linear index of first control point
            ctrl_idx = 0;
            for (int j = 0; j < mfa.p.size(); j++)
                ctrl_idx += (span[j] - mfa.p(j) + ct(0, j)) * cs[j];
            size_t start_ctrl_idx = ctrl_idx;

            for (int i = 0; i < tot_iters; i++)             // 1-d flattening all n-d nested loop computations
            {
                // always compute the point in the first dimension
                ctrl_pt = ctrl_pts.row(ctrl_idx);
                T w     = weights(ctrl_idx);

#ifdef WEIGH_ALL_DIMS                               // weigh all dimensions
                temp[0] += (N[0])(0, iter[0] + span[0] - mfa.p(0)) * ctrl_pt * w;
#else                                               // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (temp[0])(j) += (N[0])(0, iter[0] + span[0] - mfa.p(0)) * ctrl_pt(j);
                (temp[0])(last) += (N[0])(0, iter[0] + span[0] - mfa.p(0)) * ctrl_pt(last) * w;
#endif

                temp_denom(0) += w * N[0](0, iter[0] + span[0] - mfa.p(0));
                iter[0]++;

                // for all dimensions except last, check if span is finished
                ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa.p.size(); k++)
                {
                    if (i < tot_iters - 1)
                        ctrl_idx += ct(i + 1, k) * cs[k];        // ctrl_idx for the next iteration (i+1)
                    if (k < mfa.p.size() - 1 && iter[k] - 1 == mfa.p(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        temp[k + 1]        += (N[k + 1])(0, iter[k + 1] + span[k + 1] - mfa.p(k + 1)) * temp[k];
                        temp_denom(k + 1)  += temp_denom(k) * N[k + 1](0, iter[k + 1] + span[k + 1] - mfa.p(k + 1));
                        temp_denom(k)       = 0.0;
                        temp[k]             = VectorX<T>::Zero(last + 1);
                        iter[k]             = 0;
                        iter[k + 1]++;
                    }
                }
            }

            T denom;                                // rational denominator
            if (derivs.size() && derivs.sum())
                denom = 1.0;                        // TODO: weights for derivatives not implemented yet
            else
                denom = temp_denom(mfa.p.size() - 1);

#ifdef WEIGH_ALL_DIMS                           // weigh all dimensions
            out_pt = temp[mfa.p.size() - 1] / denom;
#else                                           // weigh only range dimension
            out_pt   = temp[mfa.p.size() - 1];
            out_pt(last) /= denom;
#endif

        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // fastest version for multiple points, reuses saved basis functions
        // only values, no derivatives, because basis functions were not saved for derivatives
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt_saved_basis(
                VectorXi&           ijk,        // ijk index of input domain point being decoded
                VectorX<T>&         param,      // parameter value in each dim. of desired point
                VectorX<T>&         out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&      di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                TensorProduct<T>&   tensor)     // tensor product to use for decoding
        {
            int last = tensor.ctrl_pts.cols() - 1;

            di.Reset_saved_basis(mfa);

            // linear index of first control point
            di.ctrl_idx = 0;
            for (int j = 0; j < mfa.dom_dim; j++)
            {
                di.span[j]    = mfa.FindSpan(j, param(j), tensor);
                di.ctrl_idx += (di.span[j] - mfa.p(j) + ct(0, j)) * cs[j];
            }
            size_t start_ctrl_idx = di.ctrl_idx;

            for (int i = 0; i < tot_iters; i++)             // 1-d flattening all n-d nested loop computations
            {
                // always compute the point in the first dimension
                di.ctrl_pt  = tensor.ctrl_pts.row(di.ctrl_idx);
                T w         = tensor.weights(di.ctrl_idx);

#ifdef WEIGH_ALL_DIMS                               // weigh all dimensions
                di.temp[0] += (mfa.N[0])(ijk(0), di.iter[0] + di.span[0] - mfa.p(0)) * di.ctrl_pt * w;
#else                                               // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (di.temp[0])(j) += (mfa.N[0])(ijk(0), di.iter[0] + di.span[0] - mfa.p(0)) * di.ctrl_pt(j);
                (di.temp[0])(last) += (mfa.N[0])(ijk(0), di.iter[0] + di.span[0] - mfa.p(0)) * di.ctrl_pt(last) * w;
#endif

                di.temp_denom(0) += w * mfa.N[0](ijk(0), di.iter[0] + di.span[0] - mfa.p(0));
                di.iter[0]++;

                // for all dimensions except last, check if span is finished
                di.ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa.dom_dim; k++)
                {
                    if (i < tot_iters - 1)
                        di.ctrl_idx += ct(i + 1, k) * cs[k];        // ctrl_idx for the next iteration (i+1)
                    if (k < mfa.dom_dim - 1 && di.iter[k] - 1 == mfa.p(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        di.temp[k + 1]        += (mfa.N[k + 1])(ijk(k + 1), di.iter[k + 1] + di.span[k + 1] - mfa.p(k + 1)) * di.temp[k];
                        di.temp_denom(k + 1)  += di.temp_denom(k) * mfa.N[k + 1](ijk(k + 1), di.iter[k + 1] + di.span[k + 1] - mfa.p(k + 1));
                        di.temp_denom(k)       = 0.0;
                        di.temp[k].setZero();
                        di.iter[k]             = 0;
                        di.iter[k + 1]++;
                    }
                }
            }

            T denom = di.temp_denom(mfa.dom_dim - 1);   // rational denominator

#ifdef WEIGH_ALL_DIMS                                   // weigh all dimensions
            out_pt = di.temp[mfa.dom_dim - 1] / denom;
#else                                                   // weigh only range dimension
            out_pt   = di.temp[mfa.dom_dim - 1];
            out_pt(last) /= denom;
#endif
        }

        // compute a point from a NURBS n-d volume at a given parameter value
        // faster version for multiple points, reuses decode info, but recomputes basis functions
        // algorithm 4.3, Piegl & Tiller (P&T) p.134
        void VolPt(
                VectorX<T>&         param,      // parameter value in each dim. of desired point
                VectorX<T>&         out_pt,     // (output) point, allocated by caller
                DecodeInfo<T>&      di,         // reusable decode info allocated by caller (more efficient when calling VolPt multiple times)
                TensorProduct<T>&   tensor,     // tensor product to use for decoding
                VectorXi&           derivs)     // derivative to take in each domain dim. (0 = value, 1 = 1st deriv, 2 = 2nd deriv, ...)
                                                // pass size-0 vector if unused
        {
            int last = tensor.ctrl_pts.cols() - 1;
            if (derivs.size())                  // extra check for derivatives, won't slow down normal point evaluation
            {
                if (derivs.size() != mfa.dom_dim)
                {
                    fprintf(stderr, "Error: size of derivatives vector is not the same as the number of domain dimensions\n");
                    exit(0);
                }
                for (auto i = 0; i < mfa.dom_dim; i++)
                    if (derivs(i) > mfa.p(i))
                        fprintf(stderr, "Warning: In dimension %d, trying to take derivative %d of an MFA with degree %d will result in 0. This may not be what you want",
                                i, derivs(i), mfa.p(i));
            }

            di.Reset(mfa, derivs);

            // basis funs
            for (size_t i = 0; i < mfa.dom_dim; i++)       // for all dims
            {
                di.span[i]    = mfa.FindSpan(i, param(i), tensor);

                if (derivs.size() && derivs(i))
                {
#ifndef TMESH       // original version for one tensor product
                    mfa.DerBasisFuns(i, param(i), di.span[i], derivs(i), di.ders[i]);
                    di.N[i].row(0) = di.ders[i].row(derivs(i));
#endif
                }
                else
                {
#ifndef TMESH       // original version for one tensor product
                    mfa.OrigBasisFuns(i, param(i), di.span[i], di.N[i], 0);
#else               // tmesh version
                    mfa.BasisFuns(i, param(i), di.span[i], di.N[i], 0);
#endif
                }
            }

            // linear index of first control point
            di.ctrl_idx = 0;
            for (int j = 0; j < mfa.dom_dim; j++)
                di.ctrl_idx += (di.span[j] - mfa.p(j) + ct(0, j)) * cs[j];
            size_t start_ctrl_idx = di.ctrl_idx;

            for (int i = 0; i < tot_iters; i++)             // 1-d flattening all n-d nested loop computations
            {
                // always compute the point in the first dimension
                di.ctrl_pt  = tensor.ctrl_pts.row(di.ctrl_idx);
                T w         = tensor.weights(di.ctrl_idx);

#ifdef WEIGH_ALL_DIMS                               // weigh all dimensions
                di.temp[0] += (di.N[0])(0, di.iter[0] + di.span[0] - mfa.p(0)) * di.ctrl_pt * w;
#else                                               // weigh only range dimension
                for (auto j = 0; j < last; j++)
                    (di.temp[0])(j) += (di.N[0])(0, di.iter[0] + di.span[0] - mfa.p(0)) * di.ctrl_pt(j);
                (di.temp[0])(last) += (di.N[0])(0, di.iter[0] + di.span[0] - mfa.p(0)) * di.ctrl_pt(last) * w;
#endif

                di.temp_denom(0) += w * di.N[0](0, di.iter[0] + di.span[0] - mfa.p(0));
                di.iter[0]++;

                // for all dimensions except last, check if span is finished
                di.ctrl_idx = start_ctrl_idx;
                for (size_t k = 0; k < mfa.dom_dim; k++)
                {
                    if (i < tot_iters - 1)
                        di.ctrl_idx += ct(i + 1, k) * cs[k];        // ctrl_idx for the next iteration (i+1)
                    if (k < mfa.dom_dim - 1 && di.iter[k] - 1 == mfa.p(k))
                    {
                        // compute point in next higher dimension and reset computation for current dim
                        di.temp[k + 1]        += (di.N[k + 1])(0, di.iter[k + 1] + di.span[k + 1] - mfa.p(k + 1)) * di.temp[k];
                        di.temp_denom(k + 1)  += di.temp_denom(k) * di.N[k + 1](0, di.iter[k + 1] + di.span[k + 1] - mfa.p(k + 1));
                        di.temp_denom(k)       = 0.0;
                        di.temp[k].setZero();
                        di.iter[k]             = 0;
                        di.iter[k + 1]++;
                    }
                }
            }

            T denom;                                // rational denominator
            if (derivs.size() && derivs.sum())
                denom = 1.0;                        // TODO: weights for derivatives not implemented yet
            else
                denom = di.temp_denom(mfa.dom_dim - 1);

#ifdef WEIGH_ALL_DIMS                           // weigh all dimensions
            out_pt = di.temp[mfa.dom_dim - 1] / denom;
#else                                           // weigh only range dimension
            out_pt   = di.temp[mfa.dom_dim - 1];
            out_pt(last) /= denom;
#endif
        }

        // compute a point from a NURBS curve at a given parameter value
        // this version takes a temporary set of control points for one curve only rather than
        // reading full n-d set of control points from the mfa
        // algorithm 4.1, Piegl & Tiller (P&T) p.124
        void CurvePt(
                int                     cur_dim,        // current dimension
                T                       param,          // parameter value of desired point
                MatrixX<T>&             temp_ctrl,      // temporary control points
                VectorX<T>&             temp_weights,   // weights associate with temporary control points
                const TensorProduct<T>& tensor,         // current tensor product
                VectorX<T>&             out_pt)         // (output) point
        {
            int span   = mfa.FindSpan(cur_dim, param, tensor);
            MatrixX<T> N = MatrixX<T>::Zero(1, temp_ctrl.rows());      // basis coefficients
#ifndef TMESH                                           // original version for one tensor product
            mfa.OrigBasisFuns(cur_dim, param, span, N, 0);
#else                                                   // tmesh version
            mfa.BasisFuns(cur_dim, param, span, N, 0);
#endif
            out_pt = VectorX<T>::Zero(temp_ctrl.cols());  // initializes and resizes

            for (int j = 0; j <= mfa.p(cur_dim); j++)
                out_pt += N(0, j + span - mfa.p(cur_dim)) *
                    temp_ctrl.row(span - mfa.p(cur_dim) + j) *
                    temp_weights(span - mfa.p(cur_dim) + j);

            // compute the denominator of the rational curve point and divide by it
            // sum of element-wise multiplication requires transpose so that both arrays are same shape
            // (rows in this case), otherwise eigen cannot multiply them
            T denom = (N.row(0).cwiseProduct(temp_weights.transpose())).sum();
            out_pt /= denom;
        }

    };
}

#endif
