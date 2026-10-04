// CMA-ES (normalized coordinates: x = x0 + scale * z)
struct Cma {
    int n;
    std::vector<double> m, ps, pc, D;
    std::vector<std::vector<double>> Cm, B;
    double sigma;
    int lambda, mu;
    std::vector<double> w;
    double mueff, cs, ds, cc, c1, cmu, chiN;
    int eigenGen = 0;
    Cma(int n_, double s0) : n(n_), sigma(s0) {
        m.assign(n, 0.0);
        ps.assign(n, 0.0);
        pc.assign(n, 0.0);
        D.assign(n, 1.0);
        Cm.assign(n, std::vector<double>(n, 0.0));
        B.assign(n, std::vector<double>(n, 0.0));
        for (int i = 0; i < n; ++i) Cm[i][i] = B[i][i] = 1.0;
        lambda = 4 + int(3 * std::log(double(n)));
        lambda = std::max(lambda, 16);
        mu = lambda / 2;
        w.resize(mu);
        double s = 0;
        for (int i = 0; i < mu; ++i) s += (w[i] = std::log(mu + 0.5) - std::log(i + 1.0));
        double s2 = 0;
        for (double& x : w) { x /= s; s2 += x * x; }
        mueff = 1.0 / s2;
        cs = (mueff + 2) / (n + mueff + 5);
        ds = 1 + 2 * std::max(0.0, std::sqrt((mueff - 1) / (n + 1)) - 1) + cs;
        cc = (4 + mueff / n) / (n + 4 + 2 * mueff / n);
        c1 = 2 / ((n + 1.3) * (n + 1.3) + mueff);
        cmu = std::min(1 - c1, 2 * (mueff - 2 + 1 / mueff) / ((n + 2) * (n + 2) + mueff));
        chiN = std::sqrt(double(n)) * (1 - 1.0 / (4 * n) + 1.0 / (21.0 * n * n));
    }
    void eigen() {
        // Jacobi eigen decomposition of Cm -> B (columns), D (sqrt of eigenvalues).
        std::vector<std::vector<double>> A = Cm, V(n, std::vector<double>(n, 0.0));
        for (int i = 0; i < n; ++i) V[i][i] = 1;
        for (int sweep = 0; sweep < 60; ++sweep) {
            double off = 0;
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n; ++j) off += A[i][j] * A[i][j];
            if (off < 1e-22) break;
            for (int p = 0; p < n; ++p)
                for (int q = p + 1; q < n; ++q) {
                    if (std::fabs(A[p][q]) < 1e-300) continue;
                    double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                    double t = (th >= 0 ? 1 : -1) / (std::fabs(th) + std::sqrt(th * th + 1));
                    double cth = 1 / std::sqrt(t * t + 1), sth = t * cth;
                    for (int k = 0; k < n; ++k) {
                        double akp = A[k][p], akq = A[k][q];
                        A[k][p] = cth * akp - sth * akq;
                        A[k][q] = sth * akp + cth * akq;
                    }
                    for (int k = 0; k < n; ++k) {
                        double apk = A[p][k], aqk = A[q][k];
                        A[p][k] = cth * apk - sth * aqk;
                        A[q][k] = sth * apk + cth * aqk;
                    }
                    for (int k = 0; k < n; ++k) {
                        double vkp = V[k][p], vkq = V[k][q];
                        V[k][p] = cth * vkp - sth * vkq;
                        V[k][q] = sth * vkp + cth * vkq;
                    }
                }
        }
        for (int i = 0; i < n; ++i) D[i] = std::sqrt(std::max(1e-20, A[i][i]));
        B = V;
    }
    void sample(std::mt19937& rng, std::vector<std::vector<double>>& Z, std::vector<std::vector<double>>& Y) {
        std::normal_distribution<double> N01;
        Z.assign(lambda, std::vector<double>(n));
        Y.assign(lambda, std::vector<double>(n));
        for (int k = 0; k < lambda; ++k) {
            for (int i = 0; i < n; ++i) Z[k][i] = N01(rng);
            for (int i = 0; i < n; ++i) {
                double s = 0;
                for (int j = 0; j < n; ++j) s += B[i][j] * D[j] * Z[k][j];
                Y[k][i] = s;
            }
        }
    }
    void update(const std::vector<std::vector<double>>& Y, const std::vector<std::vector<double>>& Z, const std::vector<int>& order, int gen) {
        std::vector<double> yw(n, 0.0), zw(n, 0.0);
        for (int i = 0; i < mu; ++i)
            for (int k = 0; k < n; ++k) {
                yw[k] += w[i] * Y[order[i]][k];
                zw[k] += w[i] * Z[order[i]][k];
            }
        for (int k = 0; k < n; ++k) m[k] += sigma * yw[k];
        // C^-1/2 yw = B zw
        std::vector<double> bz(n, 0.0);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) bz[i] += B[i][j] * zw[j];
        double psn = 0;
        for (int k = 0; k < n; ++k) {
            ps[k] = (1 - cs) * ps[k] + std::sqrt(cs * (2 - cs) * mueff) * bz[k];
            psn += ps[k] * ps[k];
        }
        psn = std::sqrt(psn);
        bool hs = psn / std::sqrt(1 - std::pow(1 - cs, 2.0 * (gen + 1))) / chiN < 1.4 + 2.0 / (n + 1);
        for (int k = 0; k < n; ++k) pc[k] = (1 - cc) * pc[k] + (hs ? std::sqrt(cc * (2 - cc) * mueff) * yw[k] : 0.0);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                double rmu = 0;
                for (int k = 0; k < mu; ++k) rmu += w[k] * Y[order[k]][i] * Y[order[k]][j];
                Cm[i][j] = (1 - c1 - cmu) * Cm[i][j] + c1 * (pc[i] * pc[j] + (hs ? 0 : cc * (2 - cc) * Cm[i][j])) + cmu * rmu;
            }
        sigma *= std::exp((cs / ds) * (psn / chiN - 1));
        sigma = std::min(sigma, 3.0);
        if (++eigenGen >= 1) {
            eigen();
            eigenGen = 0;
        }
    }
};

