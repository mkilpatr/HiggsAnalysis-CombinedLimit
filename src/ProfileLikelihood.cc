#include "../interface/ProfileLikelihood.h"
#include "RooRealVar.h"
#include "RooArgSet.h"
#include "RooRandom.h"
#include "RooDataSet.h"
#include "RooFitResult.h"
#include "RooMinimizer.h"
#include "TCanvas.h"
#include "RooStats/ProfileLikelihoodCalculator.h"
#include "RooStats/LikelihoodInterval.h"
#include "RooStats/LikelihoodIntervalPlot.h"
#include "RooStats/HypoTestResult.h"
#include "RooStats/RooStatsUtils.h"
#include "../interface/Combine.h"
#include "../interface/CloseCoutSentry.h"
#include "../interface/utils.h"
#include "../interface/ProfiledLikelihoodRatioTestStatExt.h"


#include <Math/MinimizerOptions.h>

using namespace RooStats;

std::string ProfileLikelihood::minimizerAlgo_ = "Minuit2";
float       ProfileLikelihood::minimizerTolerance_ = 1e-2;
int         ProfileLikelihood::tries_ = 1;
int         ProfileLikelihood::maxTries_ = 1;
float       ProfileLikelihood::maxRelDeviation_ = 0.05;
float       ProfileLikelihood::maxOutlierFraction_ = 0.25;
int         ProfileLikelihood::maxOutliers_ = 3;
bool        ProfileLikelihood::preFit_ = false;
bool        ProfileLikelihood::useMinos_ = false;
bool        ProfileLikelihood::bruteForce_ = false;
bool        ProfileLikelihood::reportPVal_ = false;
float       ProfileLikelihood::signalForSignificance_ = 0;
std::string ProfileLikelihood::plot_ = "";

ProfileLikelihood::ProfileLikelihood() :
    LimitAlgo("Profile Likelihood specific options")
{
    options_.add_options()
        ("minimizerAlgo",      boost::program_options::value<std::string>(&minimizerAlgo_)->default_value(minimizerAlgo_), "Choice of minimizer (Minuit vs Minuit2)")
        ("minimizerTolerance", boost::program_options::value<float>(&minimizerTolerance_)->default_value(minimizerTolerance_),  "Tolerance for minimizer")
        ("tries",              boost::program_options::value<int>(&tries_)->default_value(tries_), "Compute PL limit N times, to check for numerical instabilities")
        ("maxTries",           boost::program_options::value<int>(&maxTries_)->default_value(maxTries_), "Stop trying after N attempts per point")
        ("maxRelDeviation",    boost::program_options::value<float>(&maxRelDeviation_)->default_value(maxOutlierFraction_), "Max absolute deviation of the results from the median")
        ("maxOutlierFraction", boost::program_options::value<float>(&maxOutlierFraction_)->default_value(maxOutlierFraction_), "Ignore up to this fraction of results if they're too far from the median")
        ("signalForSignificance", boost::program_options::value<float>(&signalForSignificance_)->default_value(signalForSignificance_), "Signal strength used when computing significances (default is zero, just background)")
        ("maxOutliers",        boost::program_options::value<int>(&maxOutliers_)->default_value(maxOutliers_),      "Stop trying after finding N outliers")
        ("plot",   boost::program_options::value<std::string>(&plot_)->default_value(plot_), "Save a plot of the negative log of the profiled likelihood into the specified file")
        ("preFit", "Attept a fit before running the ProfileLikelihood calculator")
        ("useMinos", "Compute PL limit using Minos directly, bypassing the ProfileLikelihoodCalculator")
        ("bruteForce", "Compute PL limit by brute force, bypassing the ProfileLikelihoodCalculator and Minos")
        ("pvalue", "Report p-value instead of significance (when running with --significance)")
    ;
}

void ProfileLikelihood::applyOptions(const boost::program_options::variables_map &vm) 
{
    useMinos_ = vm.count("useMinos");
    bruteForce_ = vm.count("bruteForce");
    reportPVal_ = vm.count("pvalue");
}

ProfileLikelihood::MinimizerSentry::MinimizerSentry(std::string &minimizerAlgo, double tolerance) :
    minimizerTypeBackup(ROOT::Math::MinimizerOptions::DefaultMinimizerType()),
    minimizerAlgoBackup(ROOT::Math::MinimizerOptions::DefaultMinimizerAlgo()),
    minimizerTollBackup(ROOT::Math::MinimizerOptions::DefaultTolerance())
{
  ROOT::Math::MinimizerOptions::SetDefaultTolerance(tolerance);
  if (minimizerAlgo.find(",") != std::string::npos) {
      size_t idx = minimizerAlgo.find(",");
      std::string type = minimizerAlgo.substr(0,idx), algo = minimizerAlgo.substr(idx+1);
      if (verbose > 1) std::cout << "Set default minimizer to " << type << ", algorithm " << algo << std::endl;
      ROOT::Math::MinimizerOptions::SetDefaultMinimizer(type.c_str(), algo.c_str());
  } else {
      if (verbose > 1) std::cout << "Set default minimizer to " << minimizerAlgo << std::endl;
      ROOT::Math::MinimizerOptions::SetDefaultMinimizer(minimizerAlgo.c_str());
  }
}

ProfileLikelihood::MinimizerSentry::~MinimizerSentry() 
{
  ROOT::Math::MinimizerOptions::SetDefaultTolerance(minimizerTollBackup);
  ROOT::Math::MinimizerOptions::SetDefaultMinimizer(minimizerTypeBackup.c_str(),minimizerAlgoBackup.empty() ? 0 : minimizerAlgoBackup.c_str());
}

bool ProfileLikelihood::run(RooWorkspace *w, RooStats::ModelConfig *mc_s, RooStats::ModelConfig *mc_b, RooAbsData &data, double &limit, double &limitErr, const double *hint) { 
  MinimizerSentry minimizerConfig(minimizerAlgo_, minimizerTolerance_);
  CloseCoutSentry sentry(verbose < 0);

  RooRealVar *r = dynamic_cast<RooRealVar *>(mc_s->GetParametersOfInterest()->first());
  bool success = false;
  std::vector<double> limits; double rMax = r->getMax();  
  std::auto_ptr<RooAbsPdf> nuisancePdf(0);
  for (int i = 0; i < maxTries_; ++i) {
      w->loadSnapshot("clean");
      if (i > 0) { // randomize starting point
        r->setMax(rMax*(0.5+RooRandom::uniform()));
        r->setVal((0.1+0.5*RooRandom::uniform())*r->getMax()); 
        if (withSystematics) { 
            if (nuisancePdf.get() == 0) nuisancePdf.reset(utils::makeNuisancePdf(*mc_s));
            RooArgSet set(*mc_s->GetNuisanceParameters()); 
            RooDataSet *randoms = nuisancePdf->generate(set, 1); 
            set = *randoms->get(0);
            if (verbose > 2) {
                std::cout << "Starting minimization from point " << std::endl;
                r->Print("V");
                set.Print("V");
            }
            delete randoms;
        }
      }
      if (preFit_) {
        CloseCoutSentry sentry(verbose < 2);
        RooFitResult *res = mc_s->GetPdf()->fitTo(data, RooFit::Save(1), RooFit::Minimizer("Minuit2"));
        if (res == 0 || res->covQual() != 3 || res->edm() > minimizerTolerance_) {
            if (verbose > 1) std::cout << "Fit failed (covQual " << (res ? res->covQual() : -1) << ", edm " << (res ? res->edm() : 0) << ")" << std::endl;
            continue;
        }
        if (verbose > 1) {
            res->Print("V");
            std::cout << "Covariance quality: " << res->covQual() << ", Edm = " << res->edm() << std::endl;
        }
        delete res;
      }
      bool thisTry = (doSignificance_ ?  runSignificance(w,mc_s,data,limit,limitErr) : runLimit(w,mc_s,data,limit,limitErr));
      if (!thisTry) continue;
      if (tries_ == 1) { success = true; break; }
      limits.push_back(limit); 
      int nresults = limits.size();
      if (nresults < tries_) continue;
      std::sort(limits.begin(), limits.end());
      double median = (nresults % 2 ? limits[nresults/2] : 0.5*(limits[nresults/2] + limits[nresults/2+1]));
      int noutlier = 0; double spreadIn = 0, spreadOut = 0;
      for (int j = 0; j < nresults; ++j) {
        double diff = fabs(limits[j]-median)/median;
        if (diff < maxRelDeviation_) { 
          spreadIn = max(spreadIn, diff); 
        } else {
          noutlier++;
          spreadOut = max(spreadOut, diff); 
        }
      }
      if (verbose > 0) {
          std::cout << "Numer of tries: " << i << "   Number of successes: " << nresults 
                    << ", Outliers: " << noutlier << " (frac = " << noutlier/double(nresults) << ")"
                    << ", Spread of non-outliers: " << spreadIn <<" / of outliers: " << spreadOut << std::endl;
      }
      if (noutlier <= maxOutlierFraction_*nresults) {
        if (verbose > 0) std::cout << " \\--> success! " << std::endl;
        success = true;
        limit   = median;
        break;
      } else if (noutlier > maxOutliers_) {
        if (verbose > 0) std::cout << " \\--> failure! " << std::endl;
        break;
      }
  }
  return success;
}

bool ProfileLikelihood::runLimit(RooWorkspace *w, RooStats::ModelConfig *mc_s, RooAbsData &data, double &limit, double &limitErr) {
  RooArgSet  poi(*mc_s->GetParametersOfInterest());
  RooRealVar *r = dynamic_cast<RooRealVar *>(poi.first());
  double rMax = r->getMax();
  bool success = false;
  CloseCoutSentry coutSentry(verbose <= 1); // close standard output and error, so that we don't flood them with minuit messages

  while (!success) {
    ProfileLikelihoodCalculator plcB(data, *mc_s, 1.0-cl);
    std::auto_ptr<LikelihoodInterval> plInterval;
    if (bruteForce_) {
        std::pair<double,double> le = upperLimitBruteForce(*mc_s->GetPdf(), data, *r, mc_s->GetNuisanceParameters(), 1e-3*minimizerTolerance_, cl); 
        limit = le.first; 
        limitErr = le.second;
    } else if (useMinos_) {
        limit = upperLimitWithMinos(*mc_s->GetPdf(), data, *r, minimizerTolerance_, cl); 
    } else {
        plInterval.reset(plcB.GetInterval());
        if (plInterval.get() == 0) break;
        limit = plInterval->UpperLimit(*r);
    }
    if (limit >= 0.75*r->getMax()) { 
      std::cout << "Limit " << r->GetName() << " < " << limit << "; " << r->GetName() << " max < " << r->getMax() << std::endl;
      if (r->getMax()/rMax > 20) break;
      r->setMax(r->getMax()*2); 
      continue;
    }
    if (limit == r->getMin()) {
      std::cerr << "ProfileLikelihoodCalculator failed (returned upper limit equal to the lower bound)" << std::endl;
      break;
    }
    success = true;
    if (!plot_.empty()) {
        TCanvas *c1 = new TCanvas("c1","c1");
        LikelihoodIntervalPlot plot(&*plInterval);
        plot.Draw();
        c1->Print(plot_.c_str());
        delete c1;
    }
  }
  coutSentry.clear();
  if (verbose >= 0) {
      if (success) {
        std::cout << "\n -- Profile Likelihood -- " << "\n";
        if (limitErr) { 
            std::cout << "Limit: " << r->GetName() << " < " << limit << " +/- " << limitErr << " @ " << cl * 100 << "% CL" << std::endl;
        } else {
            std::cout << "Limit: " << r->GetName() << " < " << limit << " @ " << cl * 100 << "% CL" << std::endl;
        }
      }
  }
  return success;
}

bool ProfileLikelihood::runSignificance(RooWorkspace *w, RooStats::ModelConfig *mc_s, RooAbsData &data, double &limit, double &limitErr) {
  RooArgSet  poi(*mc_s->GetParametersOfInterest());
  RooRealVar *r = dynamic_cast<RooRealVar *>(poi.first());

  ProfileLikelihoodCalculator plcS(data, *mc_s, 1.0-cl);
  RooArgSet nullParamValues; 
  r->setVal(signalForSignificance_);
  nullParamValues.addClone(*r); 
  plcS.SetNullParameters(nullParamValues);

  CloseCoutSentry coutSentry(verbose <= 1); // close standard output and error, so that we don't flood them with minuit messages

  if (useMinos_) {
      ProfiledLikelihoodTestStatOpt testStat(*mc_s->GetObservables(), *mc_s->GetPdf(), mc_s->GetNuisanceParameters(), 
                                                   nullParamValues, RooArgList(), RooArgList(), verbose-1);
      Double_t q0 = testStat.Evaluate(data, nullParamValues);
      limit = q0 > 0 ? sqrt(2*q0) : 0;
  } else {
      std::auto_ptr<HypoTestResult> result(plcS.GetHypoTest());
      if (result.get() == 0) return false;
      limit = result->Significance();
  }
  coutSentry.clear();
  if (limit == 0 && signbit(limit)) {
      //..... This is not an error, it just means we have a deficit of events.....
      std::cerr << "The minimum of the likelihood is for r <= " << signalForSignificance_ << ", so the significance is zero" << std::endl;
      limit = 0;
  }
  if (reportPVal_) limit = RooStats::SignificanceToPValue(limit);

  std::cout << "\n -- Profile Likelihood -- " << "\n";
  std::cout << (reportPVal_ ? "p-value of background: " : "Significance: ") << limit << std::endl;
  if (verbose > 0) {
        if (reportPVal_) std::cout << "       (Significance = " << RooStats::PValueToSignificance(limit) << ")" << std::endl;
        else             std::cout << "       (p-value = " << RooStats::SignificanceToPValue(limit) << ")" << std::endl;
  }
  return true;
}


double ProfileLikelihood::upperLimitWithMinos(RooAbsPdf &pdf, RooAbsData &data, RooRealVar &poi, double tolerance, double cl) const {
    std::auto_ptr<RooArgSet> constrainedParams(pdf.getParameters(data));
    RooStats::RemoveConstantParameters(constrainedParams.get());
    std::auto_ptr<RooAbsReal> nll(pdf.createNLL(data, RooFit::Constrain(*constrainedParams)));
    RooMinimizer minim(*nll);
    minim.setStrategy(0);
    minim.setPrintLevel(verbose-1);
    minim.setErrorLevel(0.5*TMath::ChisquareQuantile(cl,1));
    nllutils::robustMinimize(*nll, minim, verbose-1);
    int mnstatus = minim.minos(RooArgSet(poi));
    //if (mnstatus == -1 && maxTries_ > 1) return poi.getMin();
    std::auto_ptr<RooFitResult> res(minim.save());
    if (verbose > 1) res->Print("V");
    return poi.getVal() + poi.getAsymErrorHi();
}

std::pair<double,double> ProfileLikelihood::upperLimitBruteForce(RooAbsPdf &pdf, RooAbsData &data, RooRealVar &poi, const RooArgSet *nuisances, double tolerance, double cl) const {
    poi.setConstant(false);
    std::auto_ptr<RooAbsReal> nll(pdf.createNLL(data, RooFit::Constrain(*nuisances)));
    RooMinimizer minim0(*nll);
    minim0.setStrategy(0);
    minim0.setPrintLevel(-1);
    nllutils::robustMinimize(*nll, minim0, verbose-2);
    poi.setConstant(true);
    RooMinimizer minim(*nll);
    minim.setPrintLevel(-1);
    if (!nllutils::robustMinimize(*nll, minim, verbose-2)) {
        std::cerr << "Initial minimization failed. Aborting." << std::endl;
        return std::pair<double,double>(0, -1);
    }
    std::auto_ptr<RooFitResult> start(minim.save());
    double minnll = nll->getVal();
    double rval = poi.getVal() + 3*poi.getError(), rlow = poi.getVal(), rhigh = poi.getMax();
    if (rval >= rhigh) rval = 0.5*(rlow + rhigh);
    double target = minnll + 0.5*TMath::ChisquareQuantile(cl,1);
    //minim.setPrintLevel(verbose-2);
    bool fail = false;
    do {
        poi.setVal(rval);
        minim.setStrategy(0);
        bool success = nllutils::robustMinimize(*nll, minim, verbose-2);
        if (success == false) {
            std::cerr << "Minimization failed at " << poi.getVal() <<". exiting the bisection loop" << std::endl;
            fail = true; 
            break;
        }
        double nllthis = nll->getVal();
        if (verbose > 1) std::cout << "  at " << poi.GetName() << " = " << rval << ", delta(NLL) = " << (nllthis - minnll) << std::endl;
        if (fabs(nllthis - target) < tolerance) {
            return std::pair<double,double>(rval, (rhigh - rlow)*0.5);
        } else if (nllthis < target) {
            rlow = rval;
            rval = 0.5*(rval + rhigh); 
        } else {
            rhigh = rval;
            rval = 0.5*(rval + rlow); 
        }
    } while (rhigh - rlow > tolerance);
    if (fail) {
        // try do do it in small steps instead
        std::auto_ptr<RooArgSet> pars(nll->getParameters((const RooArgSet *)0));
        double dx = 0.05*poi.getError();
        *pars = start->floatParsFinal();
        rval = poi.getVal() + dx;
        do {
            poi.setVal(rval);
            minim.setStrategy(0);
            bool success = nllutils::robustMinimize(*nll, minim, verbose-2);
            if (success == false) {
                std::cerr << "Minimization failed at " << poi.getVal() <<". exiting the stepping loop" << std::endl;
                return std::pair<double,double>(poi.getVal(), (rhigh - rlow)*0.5);
            }
            double nllthis = nll->getVal();
            if (verbose > 1) std::cout << "  at " << poi.GetName() << " = " << rval << ", delta(NLL) = " << (nllthis - minnll) << std::endl;
            if (fabs(nllthis - target) < tolerance) {
                return std::pair<double,double>(rval, dx);
            } else if (nllthis < target) {
                rval += dx;
            } else {
                dx *= 0.5;
                rval -= dx;
            }
        } while (rval < poi.getMax());
        return std::pair<double,double>(poi.getMax(), 0);
    } else {
        return std::pair<double,double>(poi.getVal(), (rhigh - rlow)*0.5);
    }
}