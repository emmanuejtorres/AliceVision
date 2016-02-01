
// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.


#include "openMVG/image/image.hpp"
#include "openMVG/sfm/sfm.hpp"

/// Feature/Regions & Image describer interfaces
#include "openMVG/features/features.hpp"
#include "nonFree/sift/SIFT_describer.hpp"
#include <cereal/archives/json.hpp>
#include "openMVG/system/timer.hpp"

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"
#include "third_party/progress/progress.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>

using namespace openMVG;
using namespace openMVG::image;
using namespace openMVG::features;
using namespace openMVG::sfm;
using namespace std;

features::EDESCRIBER_PRESET stringToEnum(const std::string & sPreset)
{
  features::EDESCRIBER_PRESET preset;
  if(sPreset == "NORMAL")
    preset = features::NORMAL_PRESET;
  else
  if (sPreset == "HIGH")
    preset = features::HIGH_PRESET;
  else
  if (sPreset == "ULTRA")
    preset = features::ULTRA_PRESET;
  else
    preset = features::EDESCRIBER_PRESET(-1);
  return preset;
}


// ----------
// Dispatcher
// ----------

#ifdef __linux__
#include <unistd.h>
#include <sys/wait.h>

// This function dispatch the compute function on several sub processes
// keeping the maximum number of subprocesses under maxJobs
void dispatch(const int &maxJobs, std::function<void()> compute)
{
  static int nbJobs;
  pid_t pid = fork();
  if (pid < 0)           
  {                      
    // Something bad happened
    std::cerr << "fork failed\n";
    _exit(EXIT_FAILURE);
  }
  else if(pid == 0)
  {
#ifdef OPENMVG_USE_OPENMP
    // Disable OpenMP as we dispatch the work on multiple sub processes
    // and we don't want that each subprocess use all the cpu ressource
    omp_set_num_threads(1); 
#endif
    compute();
    _exit(EXIT_SUCCESS);
  }
  else 
  {
    nbJobs++;
    if (nbJobs && (nbJobs % maxJobs == 0))
    {
      pid_t pids;
      while(pids = waitpid(-1, NULL, 0)) 
      {
        nbJobs--;
        break;
      }
    }
  }
}

// Waits for all subprocesses to be completed
void waitForCompletion()
{
  pid_t pids;
  while(pids = waitpid(-1, NULL, 0)) 
  {
    if (errno == ECHILD)
        break;
  }
}

// Returns a map containing information about the memory usage,
// it basically reads /proc/meminfo  
std::map<std::string, unsigned long> memInfos()
{
  std::map<std::string, unsigned long> memoryInfos;
  std::ifstream meminfoFile("/proc/meminfo");
  if (meminfoFile.is_open())
  {
    std::string line;
    while(getline(meminfoFile, line))
    {
      auto separator = line.find(":"); 
      if (separator!=std::string::npos)
      {
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator+1);
        memoryInfos[key] = std::strtoul(value.c_str(), nullptr, 10);
      }
    }
  }
  return memoryInfos;
}

// Count the number of processors on the machine using /proc/cpuinfo
unsigned int countProcessors()
{
  unsigned int nprocessors = 0;
  std::ifstream cpuinfoFile("/proc/cpuinfo");
  if (cpuinfoFile.is_open())
  {
    std::string line;
    while(getline(cpuinfoFile, line))
    {
        // The line must start with the word "processor"
        if (line.compare(0, std::strlen("processor"), "processor") == 0)
            nprocessors++;
    }
  }
  return nprocessors; 
}

// Returns the number of jobs to run simultaneously if one job should run on 
// 1 proc and 2GB (1<<21) of memory 
int bestNumberOfJobs(unsigned long jobMemoryRequirement = 1 << 21)
{
  assert(jobMemoryRequirement!=0);
  auto meminfos = memInfos();
  const unsigned long available = meminfos["MemFree"] + meminfos["Buffers"] + meminfos["Cached"]; 
  const unsigned int nbslots = static_cast<unsigned int>(available/jobMemoryRequirement); 
  const unsigned int nbproc = countProcessors(); 
  return std::max(std::min(nbslots, nbproc), 1u);
}

#else // __linux__

void dispatch(const int &maxJobs, std::function<void()> compute)
{
#ifdef OPENMVG_USE_OPENMP
    omp_set_num_threads(maxJobs);
#endif
    compute();
}
void waitForCompletion() {}
int bestNumberOfJobs(unsigned long jobMemoryRequirement = 1 << 21) {return 1;}  

#endif // __linux__

/// - Compute view image description (feature & descriptor extraction)
/// - Export computed data
int main(int argc, char **argv)
{
  CmdLine cmd;

  std::string sSfM_Data_Filename;
  std::string sOutDir = "";
  bool bUpRight = false;
  std::string sImage_Describer_Method = "SIFT";
  bool bForce = false;
  std::string sFeaturePreset = "";
  // MAX_JOBS_DEFAULT is the default value for maxJobs which keeps 
  // the original behavior of the program:
  constexpr static int MAX_JOBS_DEFAULT = std::numeric_limits<int>::max();
  int maxJobs = MAX_JOBS_DEFAULT;

  // required
  cmd.add( make_option('i', sSfM_Data_Filename, "input_file") );
  cmd.add( make_option('o', sOutDir, "outdir") );
  // Optional
  cmd.add( make_option('m', sImage_Describer_Method, "describerMethod") );
  cmd.add( make_option('u', bUpRight, "upright") );
  cmd.add( make_option('f', bForce, "force") );
  cmd.add( make_option('p', sFeaturePreset, "describerPreset") );
  cmd.add( make_option('j', maxJobs, "jobs") );

  try 
  {
    if (argc == 1) throw std::string("Invalid command line parameter.");
    cmd.process(argc, argv);
  } 
  catch(const std::string& s) 
  {
    std::cerr << "Usage: " << argv[0] << '\n'
    << "[-i|--input_file] a SfM_Data file \n"
    << "[-o|--outdir path] \n"
    << "\n[Optional]\n"
    << "[-f|--force] Force to recompute data\n"
    << "[-m|--describerMethod]\n"
    << "  (method to use to describe an image):\n"
    << "   SIFT (default),\n"
    << "   AKAZE_FLOAT: AKAZE with floating point descriptors,\n"
    << "   AKAZE_MLDB:  AKAZE with binary descriptors\n"
    << "[-u|--upright] Use Upright feature 0 or 1\n"
    << "[-p|--describerPreset]\n"
    << "  (used to control the Image_describer configuration):\n"
    << "   NORMAL (default),\n"
    << "   HIGH,\n"
    << "   ULTRA: !!Can take long time!!\n"
    << "[-j|--jobs] Specifies the number of jobs to run simultaneously. Use -j 0 for automatic mode.\n"
    << std::endl;

    std::cerr << s << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << " You called : " <<std::endl
            << argv[0] << std::endl
            << "--input_file " << sSfM_Data_Filename << std::endl
            << "--outdir " << sOutDir << std::endl
            << "--describerMethod " << sImage_Describer_Method << std::endl
            << "--upright " << bUpRight << std::endl
            << "--describerPreset " << (sFeaturePreset.empty() ? "NORMAL" : sFeaturePreset) << std::endl
            << "--force " << bForce << std::endl;
  if (maxJobs != MAX_JOBS_DEFAULT)
  {
    std::cout << "--jobs " << maxJobs << std::endl;
    if (maxJobs < 0) 
    {
      std::cerr << "\nInvalid value for -j option, the value must be >= 0" << std::endl;
      return EXIT_FAILURE;
    } 
    else if (maxJobs == 0)
    {
      maxJobs = bestNumberOfJobs();
    }
  }

  if (sOutDir.empty())
  {
    std::cerr << "\nIt is an invalid output directory" << std::endl;
    return EXIT_FAILURE;
  }

  


  // Create output dir
  if (!stlplus::folder_exists(sOutDir))
  {
    if (!stlplus::folder_create(sOutDir))
    {
      std::cerr << "Cannot create output directory" << std::endl;
      return EXIT_FAILURE;
    }
  }

  //---------------------------------------
  // a. Load input scene
  //---------------------------------------
  SfM_Data sfm_data;
  if (!Load(sfm_data, sSfM_Data_Filename, ESfM_Data(VIEWS|INTRINSICS))) 
  {
    std::cerr << std::endl
      << "The input file \""<< sSfM_Data_Filename << "\" cannot be read" << std::endl;
    return false;
  }

  // b. Init the image_describer
  // - retrieve the used one in case of pre-computed features
  // - else create the desired one

  using namespace openMVG::features;
  std::unique_ptr<Image_describer> image_describer;

  const std::string sImage_describer = stlplus::create_filespec(sOutDir, "image_describer", "json");
  if (!bForce && stlplus::is_file(sImage_describer))
  {
    // Dynamically load the image_describer from the file (will restore old used settings)
    std::ifstream stream(sImage_describer.c_str());
    if (!stream.is_open())
      return false;

    try
    {
      cereal::JSONInputArchive archive(stream);
      archive(cereal::make_nvp("image_describer", image_describer));
    }
    catch (const cereal::Exception & e)
    {
      std::cerr << e.what() << std::endl
        << "Cannot dynamically allocate the Image_describer interface." << std::endl;
      return EXIT_FAILURE;
    }
  }
  else
  {
    // Create the desired Image_describer method.
    // Don't use a factory, perform direct allocation
    if (sImage_Describer_Method == "SIFT")
    {
      image_describer.reset(new SIFT_Image_describer(SiftParams(), !bUpRight));
    }
    else
    if (sImage_Describer_Method == "AKAZE_FLOAT")
    {
      image_describer.reset(new AKAZE_Image_describer(AKAZEParams(AKAZEConfig(), AKAZE_MSURF), !bUpRight));
    }
    else
    if (sImage_Describer_Method == "AKAZE_MLDB")
    {
      image_describer.reset(new AKAZE_Image_describer(AKAZEParams(AKAZEConfig(), AKAZE_MLDB), !bUpRight));
    }
    //image_describer.reset(new AKAZE_Image_describer(AKAZEParams(AKAZEConfig(), AKAZE_LIOP), !bUpRight));
    if (!image_describer)
    {
      std::cerr << "Cannot create the designed Image_describer:"
        << sImage_Describer_Method << "." << std::endl;
      return EXIT_FAILURE;
    }
    else
    {
      if (!sFeaturePreset.empty())
      if (!image_describer->Set_configuration_preset(stringToEnum(sFeaturePreset)))
      {
        std::cerr << "Preset configuration failed." << std::endl;
        return EXIT_FAILURE;
      }
    }

    // Export the used Image_describer and region type for:
    // - dynamic future regions computation and/or loading
    {
      std::ofstream stream(sImage_describer.c_str());
      if (!stream.is_open())
        return false;

      cereal::JSONOutputArchive archive(stream);
      archive(cereal::make_nvp("image_describer", image_describer));
      std::unique_ptr<Regions> regionsType;
      image_describer->Allocate(regionsType);
      archive(cereal::make_nvp("regions_type", regionsType));
    }
  }

  // Feature extraction routines
  // For each View of the SfM_Data container:
  // - if regions file exist continue,
  // - if no file, compute features
  {
    system::Timer timer;
    C_Progress_display my_progress_bar( sfm_data.GetViews().size(),
      std::cout, "\n- EXTRACT FEATURES -\n" );
    for(Views::const_iterator iterViews = sfm_data.views.begin();
        iterViews != sfm_data.views.end();
        ++iterViews, ++my_progress_bar)
    {
      const View * view = iterViews->second.get();
      const std::string sView_filename = stlplus::create_filespec(sfm_data.s_root_path,
        view->s_Img_path);
      const std::string sFeat = stlplus::create_filespec(sOutDir,
        stlplus::basename_part(sView_filename), "feat");
      const std::string sDesc = stlplus::create_filespec(sOutDir,
        stlplus::basename_part(sView_filename), "desc");

      //If features or descriptors file are missing, compute them
      if (bForce || !stlplus::file_exists(sFeat) || !stlplus::file_exists(sDesc))
      {
        auto computeFunction = [&]() {
            Image<unsigned char> imageGray;
            if (!ReadImage(sView_filename.c_str(), &imageGray)) return;

            // Compute features and descriptors and export them to files
            std::unique_ptr<Regions> regions;
            image_describer->Describe(imageGray, regions);
            image_describer->Save(regions.get(), sFeat, sDesc);
        };
        if (maxJobs != MAX_JOBS_DEFAULT)
          dispatch(maxJobs, computeFunction);
        else
          computeFunction();
      }
    }

    if (maxJobs != MAX_JOBS_DEFAULT) waitForCompletion();

    std::cout << "Task done in (s): " << timer.elapsed() << std::endl;
  }
  return EXIT_SUCCESS;
}
